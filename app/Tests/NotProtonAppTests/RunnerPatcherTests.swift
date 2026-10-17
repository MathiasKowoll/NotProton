import Foundation
import Testing

@testable import NotProtonApp

@Suite("Runner patching")
struct RunnerPatcherTests {

    // Only real CrossOver bytes can answer whether a repair works, since no tree a test builds
    // meets the pinned hashes. An APFS clone costs a second and leaves the real one alone.
    private static func healthyClone() throws -> (root: URL, build: RunnerBuild)? {
        guard let build = RunnerStore.installedBuilds().first(where: {
            RunnerPatcher.verify(build: $0, root: SupportPaths.clonedRoot(forBuild: $0.id)).isEmpty
        }) else { return nil }
        let live = SupportPaths.clonedRoot(forBuild: build.id)

        let scratch = URL(filePath: NSTemporaryDirectory())
            .appending(path: "notproton-runner-\(UUID().uuidString)")
        let copied = try Shell.run("/bin/cp", [
            "-c", "-R", live.path(percentEncoded: false), scratch.path(percentEncoded: false),
        ])

        // Past the healthy check, a clone that cannot be read is this test breaking and
        // not a machine that cannot answer, so it fails rather than skipping.
        try #require(copied.status == 0)
        try #require(RunnerPatcher.verify(build: build, root: scratch).isEmpty)

        return (scratch, build)
    }

    // The launch path used to copy this on every run, so the button is now the only thing
    // that closes the gap after an app update replaces the bridge copy.
    @Test("A builtin the bridge has replaced is reported, then repaired by the install")
    func repairsStaleBuiltin() throws {
        guard let (root, build) = try Self.healthyClone() else { return }
        defer { try? FileManager.default.removeItem(at: root) }

        let arch = RunnerPatcher.unixArch(in: root)
        let builtin = root.appending(path: "lib/wine/\(arch)/lsteamclient.so")
        try Data("not the bridge copy".utf8).write(to: builtin)

        #expect(RunnerPatcher.verify(build: build, root: root)
            == ["\(arch)/lsteamclient.so is out of date"])

        let outcome = try RunnerPatcher.install(build: build, root: root)
        #expect(outcome.builtins == ["\(arch)/lsteamclient.so"])
        #expect(RunnerPatcher.verify(build: build, root: root).isEmpty)
    }

    @Test("A builtin that was never installed is reported, then repaired by the install")
    func repairsMissingBuiltin() throws {
        guard let (root, build) = try Self.healthyClone() else { return }
        defer { try? FileManager.default.removeItem(at: root) }

        let builtin = root.appending(path: "lib/wine/i386-windows/lsteamclient.dll")
        try FileManager.default.removeItem(at: builtin)

        #expect(RunnerPatcher.verify(build: build, root: root)
            == ["i386-windows/lsteamclient.dll is missing"])

        _ = try RunnerPatcher.install(build: build, root: root)
        #expect(RunnerPatcher.verify(build: build, root: root).isEmpty)
    }

    // A runner back on stock ntdll runs games that fail their ownership check, which is
    // the failure this whole path exists to prevent.
    @Test("A runner back on stock ntdll is reported, then repaired by the install")
    func repairsStockNtdll() throws {
        guard let (root, build) = try Self.healthyClone() else { return }
        defer { try? FileManager.default.removeItem(at: root) }

        let arch = WineArch.x86_64Windows
        let live = root.appending(path: "lib/wine/\(arch.rawValue)/ntdll.dll")
        let clean = NtdllPatcher.cleanSource(inRoot: root, arch: arch)
        guard clean != live else { return }

        try Data(contentsOf: clean).write(to: live)
        #expect(RunnerPatcher.verify(build: build, root: root)
            == ["\(arch.rawValue)/ntdll.dll is not the patched copy"])

        let outcome = try RunnerPatcher.install(build: build, root: root)
        #expect(outcome.ntdll == [arch])
        #expect(Digest.sha256IfPresent(live) == build.patchedNtdll[arch])
        #expect(RunnerPatcher.verify(build: build, root: root).isEmpty)
    }

    // Without the entitlement dyld drops the overlay insert and the game still runs, so
    // nothing but this check reports it.
    @Test("A loader signed without the dyld entitlement is reported, then repaired")
    func repairsStrippedEntitlement() throws {
        guard let (root, build) = try Self.healthyClone() else { return }
        defer { try? FileManager.default.removeItem(at: root) }

        let loader = root.appending(path: "lib/wine/x86_64-unix/wine")
        let clean = Clean.copy(of: loader)
        guard clean != loader else { return }

        try #require(try Shell.run("/bin/cp", [
            "-p", clean.path(percentEncoded: false), loader.path(percentEncoded: false),
        ]).status == 0)

        #expect(RunnerPatcher.verify(build: build, root: root)
            == ["x86_64-unix/wine is missing the dyld entitlement"])

        let outcome = try RunnerPatcher.install(build: build, root: root)
        #expect(outcome.loaders == ["x86_64-unix/wine"])
        #expect(RunnerPatcher.verify(build: build, root: root).isEmpty)
    }

    // Re-running has to be free, or the button cannot be the answer to every runner
    // problem the UI reports.
    @Test("Installing into a runner that is already right writes nothing")
    func installIsIdempotent() throws {
        guard let (root, build) = try Self.healthyClone() else { return }
        defer { try? FileManager.default.removeItem(at: root) }

        let outcome = try RunnerPatcher.install(build: build, root: root)
        #expect(outcome.wroteNothing)
    }

    // The FEX flavor has only a wine.app in its arm64 directory, and a loader that is not found
    // never gets the dyld entitlement. Silent at install, it shows up as a game with no dylib.
    @Test("Loader discovery finds both the bare and the bundled layout")
    func loaderDiscoveryCoversBothLayouts() throws {
        let fm = FileManager.default
        let root = URL(filePath: NSTemporaryDirectory()).appending(path: "notproton-loaders-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: root) }

        let bundled = root.appending(path: "lib/wine/aarch64-unix/wine.app/Contents/MacOS/wine")
        let bare = root.appending(path: "lib/wine/x86_64-unix/wine")
        for file in [bundled, bare] {
            try fm.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data().write(to: file)
        }

        // A windows directory alongside them must not be mistaken for a unix loader.
        let windows = root.appending(path: "lib/wine/x86_64-windows/wine")
        try fm.createDirectory(at: windows.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data().write(to: windows)

        // Compared by name rather than by URL, since the temporary directory is reached
        // through a symlink that only one of the two sides resolves.
        let found = Set(RunnerPatcher.unixLoaders(in: root).map(RunnerPatcher.name(of:)))
        #expect(found == ["aarch64-unix/wine.app/Contents/MacOS/wine", "x86_64-unix/wine"])
    }

    // The FEX flavor ships both unix directories and RUN_SCRIPT runs the arm64 loader, so
    // the unix builtin installed anywhere else is one verify_runner reads as missing.
    @Test("The unix builtin follows the loader RUN_SCRIPT picks")
    func unixBuiltinFollowsTheLoader() throws {
        let fm = FileManager.default
        let root = URL(filePath: NSTemporaryDirectory()).appending(path: "notproton-unixarch-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: root) }

        func write(_ path: String) throws {
            let file = root.appending(path: path)
            try fm.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data().write(to: file)
        }

        try write("lib/wine/x86_64-unix/wine")
        #expect(RunnerPatcher.unixArch(in: root) == "x86_64-unix")

        try write("lib/wine/aarch64-unix/wine.app/Contents/MacOS/wine")
        #expect(RunnerPatcher.unixArch(in: root) == "aarch64-unix")

        #expect(RunnerPatcher.unixArches(in: root) == ["aarch64-unix", "x86_64-unix"])

        let arches = RunnerPatcher.builtins(in: root).map(\.arch)
        #expect(
            arches == RunnerPatcher.windowsBuiltins.map(\.arch) + ["aarch64-unix", "x86_64-unix"])
        #expect(RunnerPatcher.builtins(in: root).allSatisfy { $0.name.hasPrefix("lsteamclient") })
    }

    private static func audioRunner(_ label: String) throws -> (work: URL, root: URL, bridge: URL, dir: URL) {
        let work = try scratchDirectory(label)
        let root = work.appending(path: "runner")
        let bridge = work.appending(path: "bridge")
        let dir = root.appending(path: "lib/wine/x86_64-windows")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(
            at: bridge.appending(path: "x86_64-windows"), withIntermediateDirectories: true)
        return (work, root, bridge, dir)
    }

    @Test("The audio piece takes mmdevapi.dll's place and Wine's own stays beside it")
    func installsAudioPiece() throws {
        let (work, root, bridge, dir) = try Self.audioRunner("audio-piece")
        defer { try? FileManager.default.removeItem(at: work) }
        var wine = Data(count: 0x60) + Data("wine's mmdevapi".utf8)
        wine.replaceSubrange(0x40..<0x50, with: Data("Wine builtin DLL".utf8))
        var plain = wine
        plain.replaceSubrange(0x40..<0x50, with: Data(count: 16))
        try wine.write(to: dir.appending(path: "mmdevapi.dll"))
        try Data("the piece".utf8).write(to: bridge.appending(path: "x86_64-windows/mmdevapi.dll"))

        #expect(RunnerPatcher.audioPieceProblems(root: root, bridge: bridge) == [
            "x86_64-windows/mmdevapi.dll is out of date", "x86_64-windows/mmdevapi_np.dll is missing",
        ])
        #expect(try RunnerPatcher.installAudioPiece(root: root, bridge: bridge)
            == ["x86_64-windows/mmdevapi_np.dll", "x86_64-windows/mmdevapi.dll"])
        #expect(try Data(contentsOf: dir.appending(path: "mmdevapi.dll")) == Data("the piece".utf8))
        // Kept without the builtin signature, so Wine maps that file and not the piece.
        #expect(try Data(contentsOf: dir.appending(path: "mmdevapi_np.dll")) == plain)
        #expect(try Data(contentsOf: dir.appending(path: "mmdevapi.dll.notproton-orig")) == wine)
        #expect(RunnerPatcher.audioPieceProblems(root: root, bridge: bridge).isEmpty)
        #expect(try RunnerPatcher.installAudioPiece(root: root, bridge: bridge).isEmpty)

        try Data("a newer piece".utf8).write(to: bridge.appending(path: "x86_64-windows/mmdevapi.dll"))
        #expect(RunnerPatcher.audioPieceProblems(root: root, bridge: bridge)
            == ["x86_64-windows/mmdevapi.dll is out of date"])
        #expect(try RunnerPatcher.installAudioPiece(root: root, bridge: bridge)
            == ["x86_64-windows/mmdevapi.dll"])
        #expect(try Data(contentsOf: dir.appending(path: "mmdevapi_np.dll")) == plain)
    }

    // The piece forwards to mmdevapi_np.dll: made from the piece itself, it would call itself.
    @Test("Without Wine's own mmdevapi kept, the piece is not installed")
    func refusesAudioPieceWithoutWinesOwn() throws {
        let (work, root, bridge, dir) = try Self.audioRunner("audio-piece-lost")
        defer { try? FileManager.default.removeItem(at: work) }
        try Data("the piece".utf8).write(to: bridge.appending(path: "x86_64-windows/mmdevapi.dll"))
        try Data("the piece".utf8).write(to: dir.appending(path: "mmdevapi.dll"))

        #expect(throws: StepFailure.self) { try RunnerPatcher.installAudioPiece(root: root, bridge: bridge) }
        #expect(!FileManager.default.fileExists(atPath: dir.appending(path: "mmdevapi_np.dll").path))
        #expect(RunnerPatcher.audioPieceProblems(root: root, bridge: bridge)
            == ["x86_64-windows/mmdevapi_np.dll is missing"])
    }
}
