#include "kernel_ebpf.hpp"

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(GO2CPP_HAVE_EBPF)
#include <unistd.h>
#endif

namespace go2cpp::os::monitor::detail {

bool KernelBpfUsable() {
#if defined(GO2CPP_HAVE_EBPF)
    // 非 root 在 unprivileged_bpf_disabled=2 的环境下连 open 都会被拒；
    // 提前用 euid 判断，reason 比 libbpf 的 errno 更可读。
    return ::geteuid() == 0;
#else
    return false;
#endif
}

std::string KernelBpfUnusableReason() {
#if defined(GO2CPP_HAVE_EBPF)
    if (::geteuid() != 0) {
        return "requires root privileges (euid != 0); eBPF tracepoint load is "
               "disabled";
    }
    return {};
#else
    return "go2cpp built without libbpf/clang BPF support "
           "(GO2CPP_HAVE_EBPF=0)";
#endif
}

struct KernelBpfObject::Impl {
    ~Impl() { Close(); }

    void Close() {
#if defined(GO2CPP_HAVE_EBPF)
        for (bpf_link* link : links) {
            (void)bpf_link__destroy(link);
        }
        links.clear();
        program_fds.clear();
        if (object != nullptr) {
            bpf_object__close(object);
            object = nullptr;
        }
#endif
        loaded_flag = false;
    }

#if defined(GO2CPP_HAVE_EBPF)
    bpf_object* object{nullptr};
    std::vector<bpf_link*> links;
    std::vector<int> program_fds;
#endif
    bool loaded_flag{false};
};

KernelBpfObject::KernelBpfObject() : impl_(std::make_unique<Impl>()) {}

KernelBpfObject::~KernelBpfObject() = default;

bool KernelBpfObject::Load(const unsigned char* elf, std::size_t len,
                           std::string* error) {
#if defined(GO2CPP_HAVE_EBPF)
    if (!KernelBpfUsable()) {
        if (error != nullptr) {
            *error = KernelBpfUnusableReason();
        }
        return false;
    }
    if (elf == nullptr || len == 0) {
        if (error != nullptr) {
            *error = "embedded BPF ELF is empty";
        }
        return false;
    }
    bpf_object* object = bpf_object__open_mem(elf, len, nullptr);
    const long open_error = libbpf_get_error(object);
    if (open_error != 0) {
        if (error != nullptr) {
            *error = std::string("bpf_object__open_mem failed: ") +
                     std::strerror(static_cast<int>(-open_error));
        }
        return false;
    }
    if (bpf_object__load(object) != 0) {
        if (error != nullptr) {
            *error = std::string("bpf_object__load failed: ") +
                     std::strerror(errno);
        }
        bpf_object__close(object);
        return false;
    }
    impl_->Close();
    impl_->object = object;
    impl_->loaded_flag = true;
    return true;
#else
    (void)elf;
    (void)len;
    if (error != nullptr) {
        *error = KernelBpfUnusableReason();
    }
    return false;
#endif
}

int KernelBpfObject::MapFd(const char* name) const {
#if defined(GO2CPP_HAVE_EBPF)
    if (impl_->object == nullptr || name == nullptr) {
        return -1;
    }
    return bpf_object__find_map_fd_by_name(impl_->object, name);
#else
    (void)name;
    return -1;
#endif
}

bool KernelBpfObject::AttachTracepoint(const char* program,
                                       const char* category, const char* name,
                                       std::string* error) {
#if defined(GO2CPP_HAVE_EBPF)
    if (impl_->object == nullptr) {
        if (error != nullptr) {
            *error = "BPF object is not loaded";
        }
        return false;
    }
    bpf_program* prog =
        bpf_object__find_program_by_name(impl_->object, program);
    if (prog == nullptr) {
        if (error != nullptr) {
            *error = std::string("program not found: ") + program;
        }
        return false;
    }
    bpf_link* link = bpf_program__attach_tracepoint(prog, category, name);
    const long attach_error = libbpf_get_error(link);
    if (attach_error != 0) {
        if (error != nullptr) {
            *error = std::string("attach tracepoint ") + category + ":" + name +
                     " failed: " +
                     std::strerror(static_cast<int>(-attach_error));
        }
        return false;
    }
    impl_->links.push_back(link);
    impl_->program_fds.push_back(bpf_program__fd(prog));
    return true;
#else
    (void)program;
    (void)category;
    (void)name;
    if (error != nullptr) {
        *error = KernelBpfUnusableReason();
    }
    return false;
#endif
}

bool KernelBpfObject::AttachKprobe(const char* program, const char* function,
                                   bool retprobe, std::string* error) {
#if defined(GO2CPP_HAVE_EBPF)
    if (impl_->object == nullptr) {
        if (error != nullptr) {
            *error = "BPF object is not loaded";
        }
        return false;
    }
    bpf_program* prog =
        bpf_object__find_program_by_name(impl_->object, program);
    if (prog == nullptr) {
        if (error != nullptr) {
            *error = std::string("program not found: ") + program;
        }
        return false;
    }
    struct bpf_kprobe_opts opts = {};
    opts.sz = sizeof(opts);
    opts.retprobe = retprobe;
    bpf_link* link = bpf_program__attach_kprobe_opts(prog, function, &opts);
    const long attach_error = libbpf_get_error(link);
    if (attach_error != 0) {
        if (error != nullptr) {
            *error = std::string("attach ") +
                     (retprobe ? "kretprobe " : "kprobe ") + function +
                     " failed: " +
                     std::strerror(static_cast<int>(-attach_error));
        }
        return false;
    }
    impl_->links.push_back(link);
    impl_->program_fds.push_back(bpf_program__fd(prog));
    return true;
#else
    (void)program;
    (void)function;
    (void)retprobe;
    if (error != nullptr) {
        *error = KernelBpfUnusableReason();
    }
    return false;
#endif
}

void KernelBpfObject::DetachLast(std::size_t count) {
#if defined(GO2CPP_HAVE_EBPF)
    while (count > 0 && !impl_->links.empty()) {
        bpf_link* link = impl_->links.back();
        impl_->links.pop_back();
        (void)bpf_link__destroy(link);
        if (!impl_->program_fds.empty()) {
            impl_->program_fds.pop_back();
        }
        --count;
    }
#else
    (void)count;
#endif
}

void KernelBpfObject::Close() { impl_->Close(); }

bool KernelBpfObject::loaded() const noexcept { return impl_->loaded_flag; }

void KernelBpfObject::ProgramCost(std::uint64_t* run_time_ns,
                                  std::uint64_t* run_cnt) const {
    if (run_time_ns != nullptr) {
        *run_time_ns = 0;
    }
    if (run_cnt != nullptr) {
        *run_cnt = 0;
    }
#if defined(GO2CPP_HAVE_EBPF)
    for (const int fd : impl_->program_fds) {
        struct bpf_prog_info info {};
        __u32 length = sizeof(info);
        if (bpf_prog_get_info_by_fd(fd, &info, &length) != 0) {
            continue;
        }
        if (run_time_ns != nullptr) {
            *run_time_ns += info.run_time_ns;
        }
        if (run_cnt != nullptr) {
            *run_cnt += info.run_cnt;
        }
    }
#endif
}

}  // namespace go2cpp::os::monitor::detail
