#pragma once

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <stdexcept>
#include <string>

namespace vmtest {

struct VirtError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// RAII wrapper around virConnectPtr.
class Connection {
public:
    explicit Connection(const std::string& uri);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    virConnectPtr get() const { return conn_; }

private:
    virConnectPtr conn_ = nullptr;
};

enum class DomState { NoState, Running, Blocked, Paused, ShuttingDown, ShutOff, Crashed, PMSuspended, Gone };
std::string to_string(DomState s);

// Builds the libvirt domain XML for a headless x86_64 test guest with a virtio
// disk and the QEMU guest-agent channel. Pure function - unit tested.
std::string build_domain_xml(const std::string& name, unsigned vcpus, unsigned mem_mib,
                             const std::string& disk_path, const std::string& disk_format);

// RAII wrapper around virDomainPtr for a transient (non-persistent) domain.
class Domain {
public:
    struct Info {
        unsigned vcpus = 0;
        unsigned long memory_kib = 0;
    };

    Domain() = default;
    Domain(virDomainPtr d, std::string name) : dom_(d), name_(std::move(name)) {}
    ~Domain();
    Domain(Domain&& o) noexcept;
    Domain& operator=(Domain&& o) noexcept;
    Domain(const Domain&) = delete;
    Domain& operator=(const Domain&) = delete;

    // Starts a transient domain. It is tied to the connection (AUTODESTROY), so a
    // crashed test run cannot leave a VM behind.
    static Domain create_transient(Connection& conn, const std::string& xml, const std::string& name);

    const std::string& name() const { return name_; }
    bool valid() const { return dom_ != nullptr; }

    DomState state() const;  // returns Gone once a transient domain has stopped
    void suspend();
    void resume();
    void shutdown();  // guest-agent shutdown, falling back to an ACPI power button
    void destroy();   // hard power off
    Info info() const;

    // Sends a JSON command over the QEMU guest-agent channel; returns the raw JSON reply.
    std::string agent_command(const std::string& json, int timeout_s) const;

private:
    virDomainPtr dom_ = nullptr;
    std::string name_;
};

}  // namespace vmtest
