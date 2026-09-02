package com.xdrac.rootfs

import android.system.Os
import android.util.Log
import java.io.File

/**
 * First-boot setup inside a freshly extracted rootfs: the proot bind mountpoints, DNS and
 * hosts, HOME, and a default login profile. Nothing that already exists is overwritten,
 * so running this again on a rootfs the user has edited is harmless.
 */
class RootfsConfigurator {

    sealed class Result {
        object Ok : Result()
        data class Failed(val reason: String) : Result()
    }

    fun configure(rootfs: File): Result = try {
        // PRoot binds /dev /proc /sys at launch; the guest needs the mountpoints to exist.
        Log.i(ShellLocator.TAG, "[CONFIG] Creating HOME + mountpoints")
        listOf("dev", "proc", "sys", "tmp", "root", "etc").forEach { File(rootfs, it).mkdirs() }
        runCatching { Os.chmod(File(rootfs, "tmp").absolutePath, 0x1FF) } // rwxrwxrwx for /tmp

        val etc = File(rootfs, "etc")
        // DNS: a proot guest has no resolvconf/systemd-resolved daemon to populate resolv.conf,
        // and Kali images ship /etc/resolv.conf as a symlink to a target that does not exist
        // here (…/run/…). writeIfAbsent would follow that dangling link and fail, leaving the
        // guest with no nameserver -> getaddrinfo returns EAI_AGAIN ("Temporary failure in name
        // resolution") while localhost still works via /etc/hosts. Fix: replace it with a real
        // file carrying working nameservers (this is the standard, non-workaround proot setup).
        Log.i(ShellLocator.TAG, "[CONFIG] Writing resolv.conf (forced)")
        forceWrite(
            File(etc, "resolv.conf"),
            "nameserver 8.8.8.8\nnameserver 1.1.1.1\n" +
            "nameserver 2001:4860:4860::8888\nnameserver 2606:4700:4700::1111\n"
        )
        // Ensure the resolver actually consults DNS (localhost-only symptom appears when the
        // hosts line lacks 'dns'). Only seeded if absent, to avoid disturbing a valid image.
        writeIfAbsent(
            File(etc, "nsswitch.conf"),
            "passwd: files\ngroup: files\nshadow: files\n" +
            "hosts: files dns\nnetworks: files\nprotocols: files\nservices: files\n"
        )
        Log.i(ShellLocator.TAG, "[CONFIG] Creating hosts")
        writeIfAbsent(File(etc, "hosts"), "127.0.0.1 localhost\n::1 localhost\n")

        // Seed a login profile only if the image ships none (writeIfAbsent). Debian/Kali
        // already provide /root/.profile that sources ~/.bashrc, so this rarely fires; when
        // it does, follow the same convention so a login bash picks up the image's themed
        // prompt from ~/.bashrc instead of falling back to a bare shell prompt.
        writeIfAbsent(
            File(rootfs, "root/.profile"),
            "export TERM=xterm-256color\n" +
            "if [ -n \"\$BASH_VERSION\" ] && [ -f \"\$HOME/.bashrc\" ]; then\n" +
            "    . \"\$HOME/.bashrc\"\n" +
            "else\n" +
            "    export PS1='\\[\\e[35m\\]\\u@Xterm\\[\\e[0m\\]:\\[\\e[36m\\]\\w\\[\\e[0m\\]\\$ '\n" +
            "fi\n"
        )

        // Suppress the Kali MOTD on the root fallback login too (dracos home gets its own).
        runCatching { File(rootfs, "root/.hushlogin").writeText("") }

        // Best-effort: provision a normal login user so the terminal starts as $ (non-root),
        // with passwordless sudo for escalation. This never fails the boot, on any error, or
        // on a rootfs without /etc/passwd, we skip and Bootstrap falls back to the root shell.
        runCatching { provisionUser(rootfs) }
            .onFailure { Log.w(ShellLocator.TAG, "[CONFIG] user provisioning skipped: ${it.message}") }

        Result.Ok
    } catch (t: Throwable) {
        Result.Failed(t.message ?: "configuration error")
    }

    /**
     * Public, idempotent entry point to (re)ensure the normal login user exists on an
     * ALREADY-provisioned rootfs. Called by Bootstrap every launch so installs configured before
     * the user-mode feature existed still gain 'dracos' + passwordless sudo. Never throws.
     */
    fun ensureUser(rootfs: File) {
        runCatching { provisionUser(rootfs) }
            .onFailure { Log.w(ShellLocator.TAG, "[CONFIG] ensureUser skipped: ${it.message}") }
    }

    /**
     * Idempotently add a normal user 'dracos' (uid/gid 1000) to a passwd-based rootfs and grant
     * passwordless sudo. Guarded: only touches files that already exist, never overwrites, and
     * appends the user line only when absent. On non-Debian/edge images it simply returns.
     */
    private fun provisionUser(rootfs: File) {
        val passwd = File(rootfs, "etc/passwd")
        if (!passwd.exists()) return                      // not a passwd distro (yet) -> skip
        val text = passwd.readText()
        val present = text.startsWith("dracos:") || text.contains("\ndracos:")
        if (!present) {
            Log.i(ShellLocator.TAG, "[CONFIG] creating normal user 'dracos'")
            appendLine(passwd, "dracos:x:1000:1000:dracXterm user:/home/dracos:/bin/bash")
            appendLine(File(rootfs, "etc/group"), "dracos:x:1000:")
            val shadow = File(rootfs, "etc/shadow")
            if (shadow.exists()) appendLine(shadow, "dracos:!:19999:0:99999:7:::")  // login pw disabled
        }
        // Passwordless sudo drop-in so `sudo` / `sudo su` escalate to root (# prompt).
        val sudoersD = File(rootfs, "etc/sudoers.d")
        if (sudoersD.isDirectory) {
            val drop = File(sudoersD, "dracos")
            if (!drop.exists()) {
                drop.writeText("dracos ALL=(ALL) NOPASSWD:ALL\n")
                runCatching { Os.chmod(drop.absolutePath, 0x120) }   // 0440, required by sudo
            }
        }
        File(rootfs, "home/dracos").mkdirs()              // in-rootfs mountpoint for the home bind
    }

    /** Append [line] to [f] exactly once (no-op if the file is missing or already contains it). */
    private fun appendLine(f: File, line: String) {
        if (!f.exists()) return
        val cur = f.readText()
        if (cur.contains(line)) return
        f.appendText(if (cur.isEmpty() || cur.endsWith("\n")) "$line\n" else "\n$line\n")
    }

    private fun writeIfAbsent(f: File, content: String) {
        if (!f.exists()) { f.parentFile?.mkdirs(); runCatching { f.writeText(content) } }
    }

    /** Write [content] as a real regular file, removing any existing regular file OR (crucially)
     *  dangling symlink at that path first, so the write cannot be redirected through a broken
     *  link. Used for resolv.conf, which images often ship as a symlink to a missing target. */
    private fun forceWrite(f: File, content: String) {
        f.parentFile?.mkdirs()
        runCatching { Os.remove(f.absolutePath) }   // removes a symlink itself, not its target
        runCatching { if (f.exists()) f.delete() }  // belt-and-braces for a plain file
        runCatching { f.writeText(content) }
    }
}
