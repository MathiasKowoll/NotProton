// Options a runner declares in notproton-options.json, beside its CrossOver tree.

import Foundation

// Each option is a variable the game's Compatibility page sets in its launch options,
// one of a few named values. What a value does (registry lines, passing the variable
// to Wine) is read by the run script from the same file; the app only needs the names
// for the page.
struct RunnerOptions: Codable, Equatable, Sendable {

    struct Choice: Codable, Equatable, Sendable {
        var label: String
        var value: String
    }

    struct Option: Codable, Equatable, Sendable {
        var label: String
        var name: String
        var choices: [Choice]
    }

    var section: String?
    var options: [Option]

    static let fileName = "notproton-options.json"

    // Nil when the runner declares nothing usable. Options the run script would skip
    // are left out here too, so the page never offers one that does nothing.
    static func read(crossOverRoot root: URL) -> RunnerOptions? {
        guard let data = try? Data(contentsOf: root.appending(path: fileName)),
              var found = try? JSONDecoder().decode(RunnerOptions.self, from: data)
        else { return nil }
        found.options = found.options.filter { !$0.choices.isEmpty && isUsableName($0.name) }
        return found.options.isEmpty ? nil : found
    }

    static func isUsableName(_ name: String) -> Bool {
        guard name.wholeMatch(of: #/[A-Za-z_][A-Za-z0-9_]*/#) != nil else { return false }
        let reserved = ["PATH", "HOME", "WINEPREFIX", "WINESERVER", "WINELOADER", "CX_ROOT", "CX_HOME"]
        return !reserved.contains(name) && !name.hasPrefix("DYLD_") && !name.hasPrefix("STEAM_")
    }

    // The page's copy, by tool name, for the tools whose runner declares options.
    // Steam reads it with the tool list, so it changes when the list does.
    static func pageFile(for tools: [InstalledTool], runners: URL) -> Data? {
        var byTool: [String: RunnerOptions] = [:]
        for tool in tools {
            let root = SupportPaths.clonedRoot(forBuild: tool.build, runners: runners)
            if let options = read(crossOverRoot: root) { byTool[tool.name] = options }
        }
        guard !byTool.isEmpty else { return nil }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return try? encoder.encode(byTool)
    }
}
