// Fails loudly when no usable Metal device exists (GitHub macOS runners are
// VMs with a paravirtual GPU). Run: xcrun swift scripts/ci/metal_check.swift
import Foundation
import Metal

guard let dev = MTLCreateSystemDefaultDevice() else {
    FileHandle.standardError.write("::error::MTLCreateSystemDefaultDevice() returned nil - no Metal on this runner\n".data(using: .utf8)!)
    exit(1)
}
print("Metal device: \(dev.name)")
print("  registryID=\(dev.registryID) unifiedMemory=\(dev.hasUnifiedMemory) lowPower=\(dev.isLowPower)")
print("  maxThreadsPerThreadgroup=\(dev.maxThreadsPerThreadgroup) maxBufferLength=\(dev.maxBufferLength)")
for fam in [(MTLGPUFamily.apple7, "apple7"), (.apple8, "apple8"), (.mac2, "mac2"), (.metal3, "metal3")] {
    print("  supportsFamily(\(fam.1))=\(dev.supportsFamily(fam.0))")
}

// Smoke-run a compute kernel end to end so a "device exists but can't run
// work" runner also fails here instead of producing black references.
let src = """
#include <metal_stdlib>
using namespace metal;
kernel void k(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = float(i) * 2.0; }
"""
do {
    let lib = try dev.makeLibrary(source: src, options: nil)
    let pso = try dev.makeComputePipelineState(function: lib.makeFunction(name: "k")!)
    let n = 64
    let buf = dev.makeBuffer(length: n * 4, options: .storageModeShared)!
    let q = dev.makeCommandQueue()!
    let cb = q.makeCommandBuffer()!
    let enc = cb.makeComputeCommandEncoder()!
    enc.setComputePipelineState(pso)
    enc.setBuffer(buf, offset: 0, index: 0)
    enc.dispatchThreads(MTLSize(width: n, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 32, height: 1, depth: 1))
    enc.endEncoding()
    cb.commit()
    cb.waitUntilCompleted()
    if let err = cb.error { throw err }
    let p = buf.contents().bindMemory(to: Float.self, capacity: n)
    guard p[63] == 126 else {
        FileHandle.standardError.write("::error::Metal compute produced wrong results (\(p[63]))\n".data(using: .utf8)!)
        exit(1)
    }
    print("Metal compute smoke test: ok")
} catch {
    FileHandle.standardError.write("::error::Metal compute smoke test failed: \(error)\n".data(using: .utf8)!)
    exit(1)
}
