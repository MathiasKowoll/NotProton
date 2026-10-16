import Foundation
import Testing
@testable import NotProtonApp

@Suite("Runner options")
struct RunnerOptionsTests {

    private static let release = SupportedRunners.all.first { $0.id == "26.3.0.39832" }!

    private static let manifest = """
    {"section":"Pad","options":[
     {"label":"Vibration","name":"PAD_VIBRATION","choices":[
       {"label":"Modern","value":"","reg":["[HKEY_LOCAL_MACHINE\\\\X]","\\"Mode\\"=dword:00000000"]},
       {"label":"Legacy","value":"legacy","reg":["[HKEY_LOCAL_MACHINE\\\\X]","\\"Mode\\"=dword:00000001"]}]},
     {"label":"No choices","name":"EMPTY","choices":[]},
     {"label":"Reserved","name":"DYLD_INSERT_LIBRARIES","choices":[{"label":"x","value":"y"}]},
     {"label":"Bad name","name":"1X","choices":[{"label":"x","value":"y"}]}
    ]}
    """

    private func makeRunners(_ builds: [RunnerBuild], manifest: String? = Self.manifest) throws -> URL {
        let runners = FileManager.default.temporaryDirectory.appending(path: "np-options-\(UUID().uuidString)")
        for build in builds {
            let root = SupportPaths.clonedRoot(forBuild: build.id, runners: runners)
            try FileManager.default.createDirectory(at: root.appending(path: "lib/wine"), withIntermediateDirectories: true)
            if let manifest { try Data(manifest.utf8).write(to: root.appending(path: RunnerOptions.fileName)) }
        }
        return runners
    }

    private func sync(_ runners: URL) throws -> Bool {
        try CompatToolList.sync(
            runners: runners, bridge: runners.appending(path: "bridge"), file: runners.appending(path: "support/tools"),
            compatTools: runners.appending(path: "compatibilitytools.d")
        )
    }

    @Test("Only options the run script would apply reach the page, without their effects")
    func readsUsableOptions() throws {
        let runners = try makeRunners([Self.release]); defer { try? FileManager.default.removeItem(at: runners) }
        let options = try #require(RunnerOptions.read(
            crossOverRoot: SupportPaths.clonedRoot(forBuild: Self.release.id, runners: runners)))
        #expect(options.section == "Pad")
        #expect(options.options.map(\.name) == ["PAD_VIBRATION"])
        #expect(options.options[0].choices.map(\.value) == ["", "legacy"])
    }

    @Test("A runner without the file, or with a broken one, declares nothing")
    func nothingDeclared() throws {
        let runners = try makeRunners([Self.release], manifest: "{oops")
        defer { try? FileManager.default.removeItem(at: runners) }
        #expect(RunnerOptions.read(crossOverRoot: SupportPaths.clonedRoot(forBuild: Self.release.id, runners: runners)) == nil)
        #expect(RunnerOptions.read(crossOverRoot: runners.appending(path: "none")) == nil)
    }

    @Test("The page file follows the tool list, and goes when no runner declares options")
    func pageFileFollowsTools() throws {
        let variant = Self.release.withVariant("pad")
        let runners = try makeRunners([variant]); defer { try? FileManager.default.removeItem(at: runners) }
        let page = SupportPaths.runnerOptions(beside: runners.appending(path: "support/tools"))

        #expect(try sync(runners))
        let written = try JSONDecoder().decode([String: RunnerOptions].self, from: Data(contentsOf: page))
        #expect(Set(written.keys) == Set(variant.tools.map(\.name)))
        #expect(!(try String(contentsOf: page, encoding: .utf8)).contains("dword"))
        #expect(try !sync(runners))

        try FileManager.default.removeItem(
            at: SupportPaths.clonedRoot(forBuild: variant.id, runners: runners).appending(path: RunnerOptions.fileName))
        #expect(try sync(runners))
        #expect(!FileManager.default.fileExists(atPath: page.path(percentEncoded: false)))
    }
}
