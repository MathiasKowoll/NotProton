// Supervise the broker and compatibility command for one direct-controller session.
import Darwin
import Foundation

struct LaunchError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}

func tick(_ seconds: Double) {
    RunLoop.current.run(until: Date(timeIntervalSinceNow: seconds))
}

func stop(_ process: Process?) {
    guard let process, process.isRunning else { return }
    process.terminate()
    let deadline = Date(timeIntervalSinceNow: 3)
    while process.isRunning && Date() < deadline { tick(0.02) }
    if process.isRunning { kill(process.processIdentifier, SIGKILL) }
    process.waitUntilExit()
}

// The broker prints one small readiness/probe object. Bound both time and size
// so a failed helper cannot hang the launcher or fill memory.
func readJSON(_ pipe: Pipe, timeout: Double) throws -> [String: Any] {
    var data = Data()
    let fd = pipe.fileHandleForReading.fileDescriptor
    let deadline = Date(timeIntervalSinceNow: timeout)
    while Date() < deadline {
        var descriptor = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
        let result = poll(&descriptor, 1, 50)
        if result < 0 && errno != EINTR {
            throw LaunchError("Cannot read controller service readiness")
        }
        if result > 0 {
            var bytes = [UInt8](repeating: 0, count: 1024)
            let n = read(fd, &bytes, bytes.count)
            if n <= 0 { break }
            data.append(contentsOf: bytes.prefix(n))
            if data.count > 65536 { throw LaunchError("Controller service response too large") }
            if let newline = data.firstIndex(of: 10) {
                guard
                    let object = try JSONSerialization.jsonObject(with: data[..<newline])
                        as? [String: Any]
                else {
                    throw LaunchError("Invalid controller service response")
                }
                return object
            }
        }
        tick(0.001)
    }
    throw LaunchError("Controller service readiness timed out or exited")
}

func writeJSON(_ object: [String: Any], to path: URL) throws {
    let bytes = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
    let temporary = path.deletingLastPathComponent().appendingPathComponent(
        ".session-\(UUID().uuidString)")
    let fd = open(temporary.path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0o600)
    guard fd >= 0 else { throw LaunchError("Cannot create controller session record") }
    defer {
        close(fd)
        try? FileManager.default.removeItem(at: temporary)
    }
    try FileHandle(fileDescriptor: fd, closeOnDealloc: false).write(contentsOf: bytes)
    guard rename(temporary.path, path.path) == 0 else {
        throw LaunchError("Cannot save controller session record")
    }
}

func execute(_ command: [String], environment: [String: String]) throws -> Never {
    for key in ProcessInfo.processInfo.environment.keys where environment[key] == nil {
        unsetenv(key)
    }
    for (key, value) in environment { setenv(key, value, 1) }
    // A signal ignored across exec stays ignored, and a shell cannot trap one
    // that was ignored when it started: the run script's TERM trap, which is
    // how Steam's Exit Game reaches the game, would never fire.
    for value in [SIGTERM, SIGINT, SIGHUP] { signal(value, SIG_DFL) }
    let arguments = command.map { strdup($0) } + [nil]
    defer { arguments.forEach { free($0) } }
    arguments.withUnsafeBufferPointer { _ = execvp(command[0], $0.baseAddress!) }
    throw LaunchError("Cannot run compatibility tool: \(String(cString: strerror(errno)))")
}

func main() throws -> Int32 {
    let command = Array(CommandLine.arguments.dropFirst())
    guard !command.isEmpty else { return 64 }
    let files = FileManager.default
    let root = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        .deletingLastPathComponent()
    var environment = ProcessInfo.processInfo.environment
    // Never inherit credentials or what an earlier session found. User options
    // (DSB_PCM, DSB_DISABLED) are retained.
    for key in [
        "DSB_PORT", "DSB_TOKEN", "DSB_RAW", "DSB_LIBRARY", "DSB_DEVICE", "DSB_VENDOR", "DSB_PRODUCT",
        "DSB_ROUTE",
    ] {
        environment.removeValue(forKey: key)
    }
    environment["DSB_SESSION"] = "1"
    let sessions = root.appendingPathComponent("sessions", isDirectory: true)
    try files.createDirectory(
        at: sessions, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let candidate = environment["STEAM_COMPAT_APP_ID"] ?? environment["SteamAppId"] ?? ""
    let app =
        !candidate.isEmpty && candidate.utf8.allSatisfy({ (48...57).contains($0) })
        ? candidate : "unknown"
    let info = sessions.appendingPathComponent(app + ".json")
    let stats = sessions.appendingPathComponent(app + "-status.json")
    // DSB_ROUTE tells the run script which way the session went, for its log:
    // a bridge that is off for a reason must not be off in silence.
    func bypass(_ reason: String) throws -> Never {
        try writeJSON(
            ["state": "bypass", "route": reason, "updated": Date().timeIntervalSince1970], to: stats
        )
        var unbridged = environment
        for key in ["DSB_DEVICE", "DSB_VENDOR", "DSB_PRODUCT"] { unbridged.removeValue(forKey: key) }
        unbridged["DSB_ROUTE"] = reason
        return try execute(command, environment: unbridged)
    }
    if environment["DSB_DISABLED"] == "1" { try bypass("disabled") }
    if environment["NOTPROTON_RAW_CONTROLLERS"] != "1" { try bypass("steam-input") }

    var brokerEnvironment = environment
    brokerEnvironment.removeValue(forKey: "DYLD_INSERT_LIBRARIES")
    // NOTPROTON_PAD_BACKEND names a backend the probe asks only when named
    // (gamecontroller, still to be measured on a pad); without it the probe
    // asks the DualSense, as before.
    let namedBackend = environment["NOTPROTON_PAD_BACKEND"].flatMap {
        !$0.isEmpty && $0.utf8.allSatisfy({ (97...122).contains($0) }) ? $0 : nil
    }
    if let named = environment["NOTPROTON_PAD_BACKEND"], namedBackend == nil {
        fputs("DualSense launch: NOTPROTON_PAD_BACKEND=\(named) not understood; ignored\n", stderr)
    }
    func probe() throws -> (eligible: Bool, usb: Int, product: String, backend: String, vendor: String) {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = root.appendingPathComponent("broker")
        process.arguments = ["--probe"] + (namedBackend.map { ["--backend", $0] } ?? [])
        process.environment = brokerEnvironment
        process.standardOutput = pipe
        try process.run()
        defer { stop(process) }
        let state = try readJSON(pipe, timeout: 5)
        guard let eligible = state["eligible"] as? Bool, let usb = state["usb"] as? Int,
            state["bluetooth"] is Int
        else { throw LaunchError("Incomplete controller probe") }
        // An older broker names neither the backend nor the model: that was a
        // standard DualSense.
        return (
            eligible, usb, state["product"] as? String ?? "0ce6",
            state["backend"] as? String ?? "dualsense", state["vendor"] as? String ?? "054c"
        )
    }
    var state: (eligible: Bool, usb: Int, product: String, backend: String, vendor: String)?
    var probeError: Error?
    for attempt in 0..<3 {
        do {
            state = try probe()
            probeError = nil
        } catch { probeError = error }
        if let state, state.eligible || state.usb > 0 { break }
        if attempt < 2 { tick(0.75) }
    }
    if probeError != nil || state == nil {
        // The bridge is an extra, not a gate: without a verified pad the game
        // starts as it would without NotProton's controller route.
        try bypass("probe-failed")
    }
    guard let state else { try bypass("probe-failed") }
    if !state.eligible {
        try bypass(state.usb > 0 ? "original-usb" : "no-single-bluetooth-dualsense")
    }

    let lock = open(
        sessions.appendingPathComponent("controller.lock").path,
        O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0o600)
    guard lock >= 0 else { try bypass("lock-unavailable") }
    defer { close(lock) }
    // A second game in direct mode starts without the bridge rather than not at
    // all: the pad belongs to the session that took it first.
    guard flock(lock, LOCK_EX | LOCK_NB) == 0 else { try bypass("bridge-busy") }
    let logURL = sessions.appendingPathComponent(app + ".log")
    let logFD = open(logURL.path, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0o600)
    guard logFD >= 0 else { throw LaunchError("Cannot open controller log") }
    let log = FileHandle(fileDescriptor: logFD, closeOnDealloc: true)
    var random = [UInt8](repeating: 0, count: 16)
    arc4random_buf(&random, random.count)
    let token = random.map { String(format: "%02x", $0) }.joined()
    // The PCM transport and the bridge inside Wine are the DualSense's.
    let pcm = state.backend == "dualsense" && environment["DSB_PCM"] != "0"
    environment["DSB_PCM"] = pcm ? "1" : "0"
    var broker: Process?
    var brokerPipe: Pipe?
    var child: Process?
    let signals = [SIGTERM, SIGINT, SIGHUP].map { value -> DispatchSourceSignal in
        signal(value, SIG_IGN)
        let source = DispatchSource.makeSignalSource(signal: value, queue: .main)
        source.setEventHandler {
            if let child, child.isRunning {
                kill(child.processIdentifier, value)
            } else {
                stop(broker)
                exit(128 + value)
            }
        }
        source.resume()
        return source
    }
    defer {
        signals.forEach { $0.cancel() }
        stop(broker)
        try? files.removeItem(at: info)
        try? log.close()
    }
    // Per-title feedback the game does not ask for itself, for titles that only
    // send motor values: NOTPROTON_PAD_RUMBLE=haptics plays them on the voice
    // coils; NOTPROTON_PAD_TRIGGERS sets trigger effects, one for both triggers
    // ("medium") or one per trigger ("R2:weapon,L2:rumble"), and per trigger a
    // second one, ending in "+L2" or "+R2", for while the other trigger is
    // pressed ("R2:resist@2,R2:weapon+L2"). The broker checks the same words;
    // anything else is left out here, so a typo cannot cost the session its
    // bridge.
    let rumbleHaptics = pcm && environment["NOTPROTON_PAD_RUMBLE"] == "haptics"
    let triggerKinds: Set<String> = [
        "off", "light", "medium", "strong", "resist", "weapon", "slope", "vibrate", "rumble",
    ]
    func validChoice(_ choice: Substring) -> Bool {
        let parts = choice.split(separator: "@", omittingEmptySubsequences: false)
        guard let kind = parts.first, triggerKinds.contains(String(kind)) else { return false }
        if parts.count == 1 { return true }
        return parts.count == 2 && kind != "off" && ["1", "2", "3", "4", "5", "6", "7", "8"].contains(parts[1])
    }
    func validTriggers(_ spec: String) -> Bool {
        if !spec.contains(":") { return validChoice(Substring(spec)) }
        var sides = Set<String>()
        for item in spec.split(separator: ",", omittingEmptySubsequences: false) {
            let parts = item.split(separator: ":", omittingEmptySubsequences: false)
            guard parts.count == 2, ["R2", "L2"].contains(parts[0].uppercased()) else { return false }
            let side = parts[0].uppercased()
            let wait = parts[1].split(separator: "+", omittingEmptySubsequences: false)
            // Each side has one effect, and one for while the other is pressed.
            guard wait.count <= 2, validChoice(wait[0]), sides.insert(side + "\(wait.count)").inserted
            else { return false }
            if wait.count == 2 {
                guard wait[0] != "off", wait[1].uppercased() == (side == "R2" ? "L2" : "R2") else {
                    return false
                }
            }
        }
        return true
    }
    let triggerSpec = environment["NOTPROTON_PAD_TRIGGERS"].flatMap { validTriggers($0) ? $0 : nil }
    if rumbleHaptics {
        fputs("DualSense launch: rumble plays on the haptic actuators (NOTPROTON_PAD_RUMBLE=haptics)\n", stderr)
    }
    if let triggerSpec {
        fputs("DualSense launch: trigger effects \(triggerSpec) (NOTPROTON_PAD_TRIGGERS)\n", stderr)
    } else if let unknown = environment["NOTPROTON_PAD_TRIGGERS"] {
        fputs("DualSense launch: NOTPROTON_PAD_TRIGGERS=\(unknown) not understood; triggers left alone\n", stderr)
    }
    func startBroker(port: Int? = nil) throws -> Int {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = root.appendingPathComponent("broker")
        var arguments = [
            "--backend", state.backend, "--token", token, "--parent", String(getpid()),
            "--status", stats.path,
        ]
        if pcm { arguments.append("--pcm") }
        if let port { arguments += ["--port", String(port)] }
        if rumbleHaptics { arguments.append("--rumble-haptics") }
        if let triggerSpec { arguments += ["--triggers", triggerSpec] }
        process.arguments = arguments
        process.environment = brokerEnvironment
        process.standardOutput = pipe
        process.standardError = log
        broker = process
        brokerPipe = pipe
        try process.run()
        let ready = try readJSON(pipe, timeout: 8)
        guard ready["ready"] as? Bool == true, let assigned = ready["port"] as? Int,
            (1024...65535).contains(assigned), port == nil || port == assigned
        else {
            throw LaunchError("Controller transport did not become ready")
        }
        return assigned
    }
    // The pad the session bridges, for the pieces inside Wine: which backend
    // drives it, and the vendor and model the game sees. An Edge stays an Edge
    // unless the title's Sony library knows only the standard pad
    // (NOTPROTON_PAD_AS=dualsense).
    let presentAsStandard =
        state.backend == "dualsense" && environment["NOTPROTON_PAD_AS"] == "dualsense"
        && state.product == "0df2"
    environment["DSB_DEVICE"] = state.backend
    environment["DSB_VENDOR"] = state.vendor
    environment["DSB_PRODUCT"] = presentAsStandard ? "0ce6" : state.product
    if presentAsStandard {
        fputs("DualSense launch: Edge presented as a standard DualSense (NOTPROTON_PAD_AS=dualsense)\n", stderr)
    }
    let port: Int
    do { port = try startBroker() } catch {
        stop(broker)
        // Its pipe would otherwise stay open in the run script and in Wine.
        try? brokerPipe?.fileHandleForReading.close()
        try? brokerPipe?.fileHandleForWriting.close()
        fputs("DualSense launch: \(error); starting without the bridge\n", stderr)
        try bypass("broker-failed")
    }
    environment["DSB_PORT"] = String(port)
    environment["DSB_TOKEN"] = token
    environment["DSB_ROUTE"] = "bridge"
    if state.backend == "dualsense" {
        environment["DSB_RAW"] = pcm ? "1:pcm" : "1"
        environment["DSB_LIBRARY"] = root.appendingPathComponent("bridge.dylib").path
    }
    func saveSession() throws {
        try writeJSON(
            [
                "pid": getpid(), "brokerPid": broker!.processIdentifier,
                "started": Date().timeIntervalSince1970,
                "prefix": (environment["STEAM_COMPAT_DATA_PATH"] ?? "") + "/pfx",
                "environment": environment.filter {
                    [
                        "DSB_PORT", "DSB_TOKEN", "DSB_RAW", "DSB_LIBRARY", "DSB_PCM", "DSB_DEVICE",
                        "DSB_VENDOR", "DSB_PRODUCT",
                    ]
                        .contains($0.key)
                },
            ], to: info)
    }
    try saveSession()
    let game = Process()
    // Commands supplied by compat_run.sh are absolute. /usr/bin/env also
    // preserves normal PATH lookup for standalone use without shell parsing.
    game.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    game.arguments = command
    game.environment = environment
    child = game
    try game.run()
    var reconnects = [Date]()
    var disabled = false
    while game.isRunning {
        if !disabled, let service = broker, !service.isRunning {
            reconnects.removeAll { Date().timeIntervalSince($0) >= 60 }
            // 10: the pad went away. 11: an output write stalled -- a congested
            // Bluetooth link, not a lost pad, so a fresh broker gets it back.
            if (service.terminationStatus == 10 || service.terminationStatus == 11)
                && reconnects.count < 3
            {
                reconnects.append(Date())
                let deadline = Date(timeIntervalSinceNow: 60)
                while game.isRunning && Date() < deadline {
                    tick(0.5)
                    guard let reconnected = try? probe(), reconnected.eligible else { continue }
                    do {
                        _ = try startBroker(port: port)
                        try saveSession()
                    } catch {
                        stop(broker)
                        disabled = true
                    }
                    break
                }
            }
            if broker?.isRunning != true {
                disabled = true
                fputs("DualSense transport stopped; reconnect and relaunch the game.\n", stderr)
                try? writeJSON(
                    [
                        "state": "disabled", "route": "transport-stopped",
                        "exitStatus": service.terminationStatus, "updated": Date().timeIntervalSince1970,
                    ], to: stats)
            }
        }
        tick(0.05)
    }
    withExtendedLifetime(brokerPipe) {}
    return game.terminationReason == .exit ? game.terminationStatus : 128 + game.terminationStatus
}

do { exit(try main()) } catch {
    fputs("DualSense launch: \(error)\n", stderr)
    exit(70)
}
