package com.dracxterm.archive

/**
 * Where a tar entry is allowed to put its bytes.
 *
 * Both extractors in this app unpack an archive fetched over the network into a
 * directory the app owns, so both need the same answer to the same question, and the
 * answer is easy to get subtly wrong. Keeping it in one place means one set of tests
 * covers both, and a fix reaches both.
 */
object ArchivePaths {

    /**
     * A link entry's target, and which kind of link carries it.
     *
     * The kind is not bookkeeping: it decides what an absolute target means, and the two
     * meanings are opposites. Collapsing them into one `String?` is what made the app
     * refuse every real distro image.
     */
    sealed class Link {
        abstract val target: String

        /**
         * A symbolic link. The target is a string stored verbatim and resolved only when
         * something walks it -- by which time PRoot has made this tree the root -- so an
         * absolute target is guest-absolute and names nothing on the host.
         */
        data class Sym(override val target: String) : Link()

        /**
         * A hard link. Tar records its target as a path within the archive, so an absolute
         * target names a host file, and `Os.link` (or the copy it falls back to) would act
         * on it for real.
         */
        data class Hard(override val target: String) : Link()
    }

    /** Archive names with backslashes or a `./` prefix, reduced to plain forward slashes. */
    fun normalise(name: String): String = name.replace('\\', '/').removePrefix("./")

    /**
     * True when [name] (already normalised) is the archive's own root directory rather than
     * something to unpack.
     *
     * `tar -c .` writes `./` as its first member, so the normalised name is the empty string,
     * and `.` reaches here from archives that spell the same thing without the slash. Both mean
     * "the directory you are already extracting into": there is nothing to create, and the
     * staging tree exists before the first entry is read.
     *
     * This is separate from [refuse] on purpose. An empty name is still refused there, because
     * anything that is NOT this entry and still has no name is a malformed header, and letting
     * it through would resolve to the staging directory itself. The root is skipped by name
     * before it is ever offered for writing; it is not made writable.
     */
    fun isArchiveRoot(name: String): Boolean = name.isEmpty() || name == "."

    /**
     * Why [path] must not be unpacked, or null when it is fine.
     *
     * A canonical-path check after the fact catches the entry itself, but not a link:
     * `Os.symlink` and `Os.link` take the target verbatim, and a target of `../../..`
     * reaches outside the staging tree without the entry name ever looking suspicious.
     *
     * An absolute *symlink* target is the one case that looks like an escape and is not.
     * A root filesystem is full of them -- /etc/alternatives/awk -> /usr/bin/mawk, and
     * every merged-usr link -- and they are absolute within the guest root this tree is
     * about to become, not within the host. Creating one writes nothing outside the tree
     * either: it stores a string. The escape it superficially resembles would have to come
     * from a later entry writing *through* the link, and that is what the canonical-path
     * check in each extractor refuses, on the entry that would actually do the writing.
     */
    fun refuse(path: String, link: Link?): String? = when {
        path.isEmpty() -> "empty entry name"
        path.startsWith("/") -> "absolute path in archive: $path"
        path.split('/').any { it == ".." } -> "path traversal in archive: $path"
        link == null -> null
        link.target.isEmpty() -> "empty link target: $path"
        link.target.startsWith("/") -> when (link) {
            is Link.Hard -> "absolute link target: $path -> ${link.target}"
            is Link.Sym -> null
        }
        // A HARD link target is recorded relative to the ARCHIVE ROOT (tar convention) and the
        // extractor resolves it as File(stagingRoot, target); so it must be validated root-relative,
        // NOT relative to the entry's own directory. Using the entry's directory (as a symlink would)
        // is too permissive: e.g. entry "a/b/c" with target "../../x" stays "safe" relative to a/b but
        // File(root, "../../x") climbs above the staging tree, letting Os.link reach a host file.
        link is Link.Hard -> if (escapes("", link.target)) "hardlink target escapes archive root: $path -> ${link.target}" else null
        // A relative SYMLINK target is resolved (later, under the guest root) relative to the link's
        // own directory, so it is checked that way.
        link is Link.Sym -> if (escapes(path, link.target)) "symlink escapes archive root: $path -> ${link.target}" else null
        else -> null
    }

    /**
     * True when resolving [target] from the directory holding [path] climbs above the root.
     * Pass an empty [path] to resolve [target] from the root itself (used for hard-link targets,
     * which tar records relative to the archive root).
     */
    fun escapes(path: String, target: String): Boolean {
        val base = path.split('/').dropLast(1).toMutableList()
        for (seg in target.split('/')) {
            when (seg) {
                "", "." -> {}
                ".." -> { if (base.isEmpty()) return true; base.removeAt(base.size - 1) }
                else -> base.add(seg)
            }
        }
        return false
    }
}
