import Foundation

/// Pure Swift retain cycle demo.
///
/// Swift classes inheriting from NSObject are ObjC runtime objects —
/// the MemoryGraph SDK detects their retain cycles via ISA + ivar scanning,
/// exactly like Objective-C objects.

@objcMembers
class SwiftViewModel: NSObject {
    var service: SwiftService?
}

@objcMembers
class SwiftService: NSObject {
    /// BUG: should be `weak var delegate` to break the cycle.
    var delegate: SwiftViewModel?
}

// MARK: - Pure Swift classes (no NSObject inheritance)

/// These are plain Swift reference types — no Objective-C base class.
/// On Apple platforms they are still heap-allocated via the Swift runtime
/// and registered in the ObjC class list, so the SDK can detect them.

class PureSwiftNodeA {
    var partner: PureSwiftNodeB?
}

class PureSwiftNodeB {
    var partner: PureSwiftNodeA?
}

/// Factory exposed to ObjC — creates pure-Swift retain cycles and
/// returns the live objects so they stay in memory.
@objcMembers
class SwiftCycleFactory: NSObject {

    /// Create `count` A↔B cycles using classes that do NOT inherit from NSObject.
    /// Returns an array of opaque references (the caller just needs to hold them).
    @objc static func createPureSwiftCycles(_ count: Int) -> NSArray {
        let result = NSMutableArray(capacity: count)
        for _ in 0..<count {
            let a = PureSwiftNodeA()
            let b = PureSwiftNodeB()
            a.partner = b   // A → B
            b.partner = a   // B → A  (cycle!)
            // Store `a`; `b` stays alive through a.partner.
            result.add(a)
        }
        return result
    }
}
