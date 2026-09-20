package com.dracxterm.ollama

import com.dracxterm.archive.ArchivePaths

/**
 * Which entries of the official `ollama-linux-arm64.tar.zst` reach the rootfs.
 *
 * The split it relies on is structural, not a name match. Upstream installs the CPU
 * runtime flat into `lib/ollama/`, and every hardware-accelerator runner into its own
 * `lib/ollama/<runner>/` subdirectory, driven by OLLAMA_RUNNER_DIR in CMakeLists.txt and
 * llama/server/CMakePresets.json. So the filter tests directories, and a CPU file whose
 * name happens to contain "cuda" is kept.
 *
 * Dropping the accelerator subdirectories is safe because discovery is a glob over them.
 * discover/runner.go globs one level below lib/ollama for a ggml backend, and matching
 * nothing is the documented zero-GPU path rather than an error. Nothing in the kept set
 * links against CUDA either: neither bin/ollama nor lib/ollama/llama-server names cuda,
 * cublas, cudart or nvidia in DT_NEEDED, and those names appear only as strings handed
 * to dlopen at runtime. The libraries could not load in any case, since ggml-cuda needs
 * libcuda.so.1 from the NVIDIA driver, which no Android handset has.
 *
 * Everything unrecognised is kept and logged, never dropped. An upstream layout change
 * then costs storage rather than a broken install, which is the trade this policy wants.
 * [Decision.DropAccelerator] carries the runner name so a future build can turn one back
 * on without touching the extractor.

 */
object OllamaPayloadPolicy {

    sealed class Decision {
        /** Required for CPU-only ARM64 execution. */
        data class Keep(val why: String) : Decision()
        /** Proven-unnecessary hardware-accelerator runner payload. */
        data class DropAccelerator(val runner: String, val why: String) : Decision()
        /** Refused for safety (tar-slip, absolute path, escaping symlink). Fails the install. */
        data class Reject(val why: String) : Decision()
    }

    /** Exact OLLAMA_RUNNER_DIR values that denote a hardware accelerator runner. Source:
     *  llama/server/CMakePresets.json at tag v0.34.2. */
    private val ACCELERATOR_RUNNERS = setOf(
        "cuda_v12", "cuda_v13",
        "cuda_jetpack5", "cuda_jetpack6",
        "rocm_v7_1", "rocm_v7_2",
        "vulkan"
    )

    /** Prefixes covering versioned accelerator runners upstream may add (cuda_v14, rocm_v8_0,
     *  mlx_cuda_v13, the last is named in ml/path.go's own comment). Still a DIRECTORY test on a
     *  runner-dir name, never a substring test on a file name. */
    private val ACCELERATOR_PREFIXES = listOf("cuda_", "rocm_", "hip_", "mlx_", "metal_")

    private fun isAcceleratorRunner(dir: String): Boolean =
        dir in ACCELERATOR_RUNNERS || ACCELERATOR_PREFIXES.any { dir.startsWith(it) }

    /**
     * Classify one tar entry path (already normalised to forward slashes, no leading "./").
     * [link] is the target and kind when the entry is a link, else null.
     */
    fun classify(path: String, link: ArchivePaths.Link?): Decision {
        // Safety first: archive metadata never decides where bytes land.
        ArchivePaths.refuse(path, link)?.let { return Decision.Reject(it) }

        val parts = path.split('/')

        // ---- ALLOW: the Ollama CLI/server executable ------------------------------------------
        if (parts.size == 2 && parts[0] == "bin") {
            return Decision.Keep("ollama executable (bin/)")
        }

        if (parts.size >= 3 && parts[0] == "lib" && parts[1] == "ollama") {
            // depth-1 under lib/ollama  => OLLAMA_RUNNER_DIR == "" => the CPU/base runtime.
            if (parts.size == 3) {
                return Decision.Keep("CPU/base runtime (lib/ollama, OLLAMA_RUNNER_DIR=\"\")")
            }
            // deeper => lib/ollama/<runner>/... => a runner subdirectory.
            val runner = parts[2]
            return if (isAcceleratorRunner(runner)) {
                Decision.DropAccelerator(
                    runner,
                    "hardware-accelerator runner dir; dynamically discovered via " +
                        "filepath.Glob(lib/ollama/*/*ggml-*) and additive to the CPU base, " +
                        "and its NVIDIA/AMD driver can never exist on Android"
                )
            } else {
                Decision.Keep("unrecognised lib/ollama subdirectory '$runner' - kept (conservative)")
            }
        }

        // ---- Anything else: keep and log. Never drop what we have not proven unnecessary. -------
        return Decision.Keep("unrecognised archive path - kept (conservative)")
    }
}
