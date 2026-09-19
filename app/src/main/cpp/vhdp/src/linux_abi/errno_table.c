#include "linux_abi/errno_table.h"

#include <errno.h>
#include <stddef.h>

struct errno_entry {
    int value;
    const char* name;
    const char* description;
};

/* Values come from the target's <errno.h> (identical on x86_64 and aarch64 Linux). */
static const struct errno_entry k_errno_table[] = {
    {EPERM, "EPERM", "Operation not permitted"},
    {ENOENT, "ENOENT", "No such file or directory"},
    {ESRCH, "ESRCH", "No such process"},
    {EINTR, "EINTR", "Interrupted system call"},
    {EIO, "EIO", "Input/output error"},
    {ENXIO, "ENXIO", "No such device or address"},
    {E2BIG, "E2BIG", "Argument list too long"},
    {ENOEXEC, "ENOEXEC", "Exec format error"},
    {EBADF, "EBADF", "Bad file descriptor"},
    {ECHILD, "ECHILD", "No child processes"},
    {EAGAIN, "EAGAIN", "Resource temporarily unavailable"},
    {ENOMEM, "ENOMEM", "Cannot allocate memory"},
    {EACCES, "EACCES", "Permission denied"},
    {EFAULT, "EFAULT", "Bad address"},
    {EBUSY, "EBUSY", "Device or resource busy"},
    {EEXIST, "EEXIST", "File exists"},
    {EXDEV, "EXDEV", "Invalid cross-device link"},
    {ENODEV, "ENODEV", "No such device"},
    {ENOTDIR, "ENOTDIR", "Not a directory"},
    {EISDIR, "EISDIR", "Is a directory"},
    {EINVAL, "EINVAL", "Invalid argument"},
    {EMFILE, "EMFILE", "Too many open files"},
    {ENOSPC, "ENOSPC", "No space left on device"},
    {ESPIPE, "ESPIPE", "Illegal seek"},
    {EROFS, "EROFS", "Read-only file system"},
    {EMLINK, "EMLINK", "Too many links"},
    {EPIPE, "EPIPE", "Broken pipe"},
    {ERANGE, "ERANGE", "Numerical result out of range"},
    {ENAMETOOLONG, "ENAMETOOLONG", "File name too long"},
    {ENOSYS, "ENOSYS", "Function not implemented"},
    {ENOTEMPTY, "ENOTEMPTY", "Directory not empty"},
    {ELOOP, "ELOOP", "Too many levels of symbolic links"},
    {ENODATA, "ENODATA", "No data available"},
    {EOVERFLOW, "EOVERFLOW", "Value too large for defined data type"},
    {ENOTSOCK, "ENOTSOCK", "Socket operation on non-socket"},
    {EAFNOSUPPORT, "EAFNOSUPPORT", "Address family not supported by protocol"},
    {EADDRINUSE, "EADDRINUSE", "Address already in use"},
    {ECONNREFUSED, "ECONNREFUSED", "Connection refused"},
    {EOPNOTSUPP, "EOPNOTSUPP", "Operation not supported"},
    {ETIMEDOUT, "ETIMEDOUT", "Connection timed out"},
    {ENOTTY, "ENOTTY", "Inappropriate ioctl for device"},
    {ETXTBSY, "ETXTBSY", "Text file busy"},
    {EFBIG, "EFBIG", "File too large"},
    {EDOM, "EDOM", "Numerical argument out of domain"},
    {EPROTONOSUPPORT, "EPROTONOSUPPORT", "Protocol not supported"},
    {EDEADLK, "EDEADLK", "Resource deadlock avoided"},
    {ENOTBLK, "ENOTBLK", "Block device required"},
    {ENOLCK, "ENOLCK", "No locks available"},
    {EILSEQ, "EILSEQ", "Invalid or incomplete multibyte or wide character"},
    {EDESTADDRREQ, "EDESTADDRREQ", "Destination address required"},
    {EMSGSIZE, "EMSGSIZE", "Message too long"},
    {EPROTOTYPE, "EPROTOTYPE", "Protocol wrong type for socket"},
    {ENOPROTOOPT, "ENOPROTOOPT", "Protocol not available"},
    {EADDRNOTAVAIL, "EADDRNOTAVAIL", "Cannot assign requested address"},
    {ENETDOWN, "ENETDOWN", "Network is down"},
    {ENETUNREACH, "ENETUNREACH", "Network is unreachable"},
    {ECONNRESET, "ECONNRESET", "Connection reset by peer"},
    {ENOBUFS, "ENOBUFS", "No buffer space available"},
    {EISCONN, "EISCONN", "Transport endpoint is already connected"},
    {ENOTCONN, "ENOTCONN", "Transport endpoint is not connected"},
    {EHOSTUNREACH, "EHOSTUNREACH", "No route to host"},
    {EALREADY, "EALREADY", "Operation already in progress"},
    {EINPROGRESS, "EINPROGRESS", "Operation now in progress"},
    {ESTALE, "ESTALE", "Stale file handle"},
    {EDQUOT, "EDQUOT", "Disk quota exceeded"},
    {ECANCELED, "ECANCELED", "Operation canceled"},
    {EOWNERDEAD, "EOWNERDEAD", "Owner died"},
    {ENOTRECOVERABLE, "ENOTRECOVERABLE", "State not recoverable"},
    {ENOKEY, "ENOKEY", "Required key not available"},
    {ENOMEDIUM, "ENOMEDIUM", "No medium found"},
    {ELIBBAD, "ELIBBAD", "Accessing a corrupted shared library"},
    {ENOLINK, "ENOLINK", "Link has been severed"},
    {EPROTO, "EPROTO", "Protocol error"},
    {EBADMSG, "EBADMSG", "Bad message"},
    {ENOSTR, "ENOSTR", "Device not a stream"},
    {ETIME, "ETIME", "Timer expired"},
    {ENOSR, "ENOSR", "Out of streams resources"},
    {EREMOTE, "EREMOTE", "Object is remote"},
    {EUSERS, "EUSERS", "Too many users"},
    {ESOCKTNOSUPPORT, "ESOCKTNOSUPPORT", "Socket type not supported"},
    {EPFNOSUPPORT, "EPFNOSUPPORT", "Protocol family not supported"},
    {ESHUTDOWN, "ESHUTDOWN", "Cannot send after transport endpoint shutdown"},
    {ETOOMANYREFS, "ETOOMANYREFS", "Too many references: cannot splice"},
    {EHOSTDOWN, "EHOSTDOWN", "Host is down"},
    {ECHRNG, "ECHRNG", "Channel number out of range"},
};

static const struct errno_entry* find_entry(int err) {
    for (size_t i = 0; i < sizeof(k_errno_table) / sizeof(k_errno_table[0]); ++i) {
        if (k_errno_table[i].value == err) {
            return &k_errno_table[i];
        }
    }
    return NULL;
}

const char* vhdp_errno_name(int err) {
    const struct errno_entry* e = find_entry(err);
    return e != NULL ? e->name : "E?";
}

const char* vhdp_errno_description(int err) {
    const struct errno_entry* e = find_entry(err);
    return e != NULL ? e->description : "unknown error";
}
