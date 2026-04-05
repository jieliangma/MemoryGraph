Pod::Spec.new do |s|
  s.name         = 'MemoryGraph'
  s.version      = '1.0.0'
  s.summary      = 'iOS OOM diagnostics SDK — captures memory snapshots before Jetsam kills the app.'
  s.description  = <<-DESC
    MemoryGraph captures a detailed memory snapshot (VM regions, heap objects,
    leak analysis) when iOS is about to kill your app due to excessive memory
    usage.  The snapshot is written as a compact binary report (.mgbin)
    with a text formatter for human-readable display.

    Features:
    - Two-phase OOM detection (memory pressure + threshold polling)
    - Zero-malloc collection via pre-allocated memory pool
    - Thread suspension during snapshot for data consistency
    - ObjC/Swift object identification and Top-N class ranking
    - Statistical leak analysis
    - App Store compliant mode available
  DESC

  s.homepage     = 'https://github.com/user/MemoryGraph'
  s.license      = { :type => 'MIT', :file => 'LICENSE' }
  s.author       = { 'MemoryGraph' => 'majieliang@yeah.net' }
  s.source       = { :git => 'https://github.com/user/MemoryGraph.git', :tag => s.version.to_s }

  s.ios.deployment_target = '13.0'
  s.requires_arc = false
  s.static_framework = true

  s.default_subspec = 'Full'

  # --- Full version (uses Mach APIs for detailed data) ---
  s.subspec 'Full' do |full|
    full.source_files         = 'src/**/*.{c,h}', 'include/**/*.h'
    full.public_header_files  = 'include/memory_graph.h'
    full.header_dir           = 'MemoryGraph'
    full.compiler_flags       = '-std=c11 -Wall -Wextra -pedantic -fvisibility=hidden'
    full.pod_target_xcconfig  = {
      'GCC_C_LANGUAGE_STANDARD' => 'c11',
    }
  end

  # --- App Store compliant version (public APIs only) ---
  s.subspec 'AppStoreCompliant' do |compat|
    compat.source_files         = 'src/**/*.{c,h}', 'include/**/*.h'
    compat.public_header_files  = 'include/memory_graph.h'
    compat.header_dir           = 'MemoryGraph'
    compat.compiler_flags       = '-std=c11 -Wall -Wextra -pedantic -fvisibility=hidden'
    compat.pod_target_xcconfig  = {
      'GCC_PREPROCESSOR_DEFINITIONS' => 'MG_APP_STORE_COMPLIANT=1',
      'GCC_C_LANGUAGE_STANDARD'      => 'c11',
    }
  end
end
