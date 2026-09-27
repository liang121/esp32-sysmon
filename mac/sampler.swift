// sysmon sampler: prints one JSON line per second to stdout.
// Build: swiftc -O -o sampler sampler.swift
import Foundation
import Darwin

setvbuf(stdout, nil, _IOLBF, 0)

// MARK: CPU (per-core ticks via host_processor_info)
var prevTicks: [[UInt32]] = []
func sampleCPU() -> (total: Double, cores: [Double]) {
    var n: natural_t = 0
    var info: processor_info_array_t?
    var cnt: mach_msg_type_number_t = 0
    guard host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt) == KERN_SUCCESS,
          let info else { return (0, []) }
    var ticks: [[UInt32]] = []
    for i in 0..<Int(n) {
        let b = i * Int(CPU_STATE_MAX)
        ticks.append([UInt32(bitPattern: info[b + Int(CPU_STATE_USER)]),
                      UInt32(bitPattern: info[b + Int(CPU_STATE_SYSTEM)]),
                      UInt32(bitPattern: info[b + Int(CPU_STATE_IDLE)]),
                      UInt32(bitPattern: info[b + Int(CPU_STATE_NICE)])])
    }
    vm_deallocate(mach_task_self_, vm_address_t(bitPattern: info), vm_size_t(Int(cnt) * MemoryLayout<integer_t>.size))
    var cores: [Double] = []
    var busyAll = 0.0, totalAll = 0.0
    if prevTicks.count == ticks.count {
        for i in 0..<ticks.count {
            let d = (0..<4).map { Double(ticks[i][$0] &- prevTicks[i][$0]) }
            let busy = d[0] + d[1] + d[3], total = busy + d[2]
            cores.append(total > 0 ? busy / total * 100 : 0)
            busyAll += busy; totalAll += total
        }
    }
    prevTicks = ticks
    return (totalAll > 0 ? busyAll / totalAll * 100 : 0, cores)
}

// MARK: Memory
func sysctlValue<T>(_ name: String, _ initial: T) -> T {
    var v = initial; var sz = MemoryLayout<T>.size
    sysctlbyname(name, &v, &sz, nil, 0)
    return v
}
let memTotal = Double(sysctlValue("hw.memsize", UInt64(0)))
let pageSize = Double(vm_kernel_page_size)
var prevSwapIO: (UInt64, UInt64)? = nil

func sampleMem() -> [String: Any] {
    var vm = vm_statistics64()
    var cnt = mach_msg_type_number_t(MemoryLayout<vm_statistics64>.size / MemoryLayout<integer_t>.size)
    _ = withUnsafeMutablePointer(to: &vm) {
        $0.withMemoryRebound(to: integer_t.self, capacity: Int(cnt)) {
            host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &cnt)
        }
    }
    let app = Double(vm.internal_page_count &- vm.purgeable_count) * pageSize
    let wired = Double(vm.wire_count) * pageSize
    let compressed = Double(vm.compressor_page_count) * pageSize
    // Uncompressed size of everything the compressor holds (including segments swapped to disk).
    let storedInCompressor = Double(vm.total_uncompressed_pages_in_compressor) * pageSize
    // Anonymous (app) memory resident as-is, excluding purgeable caches apps can drop.
    let anonResident = Double(vm.internal_page_count &- vm.purgeable_count) * pageSize
    let swap = sysctlValue("vm.swapusage", xsw_usage())
    let io = (vm.swapins, vm.swapouts)
    var swapRate = 0.0
    if let p = prevSwapIO { swapRate = Double((io.0 &- p.0) &+ (io.1 &- p.1)) * pageSize / 1_048_576 }
    prevSwapIO = io
    return [
        "pressure": Int(sysctlValue("kern.memorystatus_vm_pressure_level", Int32(0))), // 1 normal, 2 warn, 4 critical
        "freePct": Int(sysctlValue("kern.memorystatus_level", Int32(0))),
        "usedGB": (app + wired + compressed) / 1_073_741_824,
        "totalGB": memTotal / 1_073_741_824,
        "compressedGB": compressed / 1_073_741_824,
        "storedGB": storedInCompressor / 1_073_741_824,
        // what apps actually hold if nothing were compressed: resident anon + pre-compression size
        "demandGB": (anonResident + storedInCompressor) / 1_073_741_824,
        "swapUsedGB": Double(swap.xsu_used) / 1_073_741_824,
        "swapMBps": swapRate,
    ]
}

// MARK: Processes (top by CPU, refreshed every 2 s)
// Some tools install one binary per version (…/claude/versions/2.1.233); name those after the
// nearest path component that isn't a version number or "versions".
func displayName(_ pid: pid_t, _ name: String) -> String {
    let isVersion = { (s: String) in s.range(of: #"^\d+(\.\d+)+$"#, options: .regularExpression) != nil }
    guard isVersion(name) else { return name }
    // exec path from KERN_PROCARGS2 (still valid after an auto-update deleted the old binary,
    // unlike proc_pidpath): layout is [argc:int32][exec_path\0]...
    var mib: [Int32] = [CTL_KERN, KERN_PROCARGS2, pid]
    var size = 0
    guard sysctl(&mib, 3, nil, &size, nil, 0) == 0, size > 4 else { return name }
    var buf = [UInt8](repeating: 0, count: size)
    guard sysctl(&mib, 3, &buf, &size, nil, 0) == 0 else { return name }
    let pathBytes = buf[4...].prefix { $0 != 0 }
    let parts = String(decoding: pathBytes, as: UTF8.self).split(separator: "/").map(String.init)
    return parts.reversed().first { !isVersion($0) && $0 != "versions" } ?? name
}
var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
let tickToNs = Double(tb.numer) / Double(tb.denom)
var prevProcTime: [pid_t: UInt64] = [:]
var prevProcAt = DispatchTime.now().uptimeNanoseconds
var topProcs: [[Any]] = []
var unreadable = 0

func sampleProcs() {
    let now = DispatchTime.now().uptimeNanoseconds
    let elapsed = Double(now - prevProcAt); prevProcAt = now
    var pids = [pid_t](repeating: 0, count: 4096)
    let n = Int(proc_listallpids(&pids, Int32(pids.count * MemoryLayout<pid_t>.size)))
    var cur: [pid_t: UInt64] = [:]
    var rows: [(String, Double, Double)] = []
    var denied = 0
    for pid in pids.prefix(max(n, 0)) where pid > 0 {
        var ti = proc_taskinfo()
        let sz = Int32(MemoryLayout<proc_taskinfo>.size)
        guard proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sz) == sz else { denied += 1; continue }
        let t = ti.pti_total_user + ti.pti_total_system
        cur[pid] = t
        guard let p = prevProcTime[pid], elapsed > 0 else { continue }
        let pct = Double(t &- p) * tickToNs / elapsed * 100
        if pct < 0.5 { continue }
        var name = [CChar](repeating: 0, count: 256)
        proc_name(pid, &name, 256)
        rows.append((displayName(pid, String(cString: name)), pct, Double(ti.pti_resident_size) / 1_048_576))
    }
    prevProcTime = cur
    unreadable = denied
    topProcs = rows.sorted { $0.1 > $1.1 }.prefix(6).map { [$0.0, ($0.1 * 10).rounded() / 10, $0.2.rounded()] }
}

// MARK: Loop
_ = sampleCPU(); sampleProcs(); _ = sampleMem()
var tick = 0
while true {
    Thread.sleep(forTimeInterval: 1.0)
    tick += 1
    if tick % 2 == 0 { sampleProcs() }
    let cpu = sampleCPU()
    var load = [Double](repeating: 0, count: 3); getloadavg(&load, 3)
    let obj: [String: Any] = [
        "t": Int(Date().timeIntervalSince1970 * 1000),
        "cpu": (cpu.total * 10).rounded() / 10,
        "cores": cpu.cores.map { ($0).rounded() },
        "load": (load[0] * 100).rounded() / 100,
        "mem": sampleMem(),
        "top": topProcs,
        "hiddenProcs": unreadable,
    ]
    if let data = try? JSONSerialization.data(withJSONObject: obj), let s = String(data: data, encoding: .utf8) {
        print(s)
    }
}
