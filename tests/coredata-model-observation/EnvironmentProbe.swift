import Foundation
import CoreData
import SwiftUI
import Darwin

@MainActor
@inline(never)
private func assign<Value>(_ root: inout EnvironmentValues, _ path: WritableKeyPath<EnvironmentValues, Value>, _ value: Value) {
    root[keyPath: path] = value
}

@MainActor
private func publish(_ mode: String, _ result: String, _ facts: [String: Any]?, to url: URL) -> Bool {
    let record: [String: Any] = ["format": 1, "mode": mode, "result": result, "facts": facts as Any? ?? NSNull()]
    do {
        let data = try JSONSerialization.data(withJSONObject: record, options: [.sortedKeys])
        guard data.count <= 4096 else { return false }
        try data.write(to: url, options: .atomic)
        return true
    } catch { return false }
}

@main
private struct EnvironmentObservation {
    @MainActor
    static func main() {
        guard CommandLine.arguments.count == 3 else { exit(2) }
        let mode = CommandLine.arguments[1]
        guard ["default", "direct", "generic-fresh", "generic-seeded"].contains(mode) else { exit(2) }
        let output = URL(fileURLWithPath: CommandLine.arguments[2])
        guard publish(mode, "entered", nil, to: output) else { exit(3) }
        var environment = EnvironmentValues()
        var facts: [String: Any] = [:]
        var success = true
        if mode == "default" {
            let first = environment.managedObjectContext
            let second = environment.managedObjectContext
            let concurrency: String
            switch first.concurrencyType {
            case .confinementConcurrencyType: concurrency = "confinement"
            case .privateQueueConcurrencyType: concurrency = "private"
            case .mainQueueConcurrencyType: concurrency = "main"
            @unknown default: concurrency = "unknown"
            }
            facts = ["concurrency": concurrency, "coordinatorPresent": first.persistentStoreCoordinator != nil,
                     "parentPresent": first.parent != nil, "repeatedIdentity": first === second]
        } else {
            let first = NSManagedObjectContext(concurrencyType: .mainQueueConcurrencyType)
            if mode == "direct" {
                environment.managedObjectContext = first
                success = environment.managedObjectContext === first
                facts = ["providedIdentity": success]
            } else if mode == "generic-fresh" {
                assign(&environment, \.managedObjectContext, first)
                success = environment.managedObjectContext === first
                facts = ["providedIdentity": success]
            } else {
                let second = NSManagedObjectContext(concurrencyType: .mainQueueConcurrencyType)
                environment.managedObjectContext = first
                let seed = environment.managedObjectContext === first
                assign(&environment, \.managedObjectContext, second)
                let replaced = environment.managedObjectContext === second
                success = seed && replaced
                facts = ["seedIdentity": seed, "providedIdentity": replaced]
            }
        }
        guard publish(mode, success ? "pass" : "failed", facts, to: output) else { exit(3) }
        exit(success ? 0 : 1)
    }
}
