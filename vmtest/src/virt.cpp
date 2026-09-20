#include "vmtest/virt.hpp"

#include <libvirt/libvirt-qemu.h>

#include <cstdlib>
#include <sstream>

#include "vmtest/util.hpp"

namespace vmtest {

namespace {

std::string last_error() {
    virErrorPtr e = virGetLastError();
    return (e && e->message) ? e->message : "unknown libvirt error";
}

void silent_error_handler(void*, virErrorPtr) {}  // we report errors ourselves

}  // namespace

// ---- Connection -------------------------------------------------------------

Connection::Connection(const std::string& uri) {
    virSetErrorFunc(nullptr, silent_error_handler);
    conn_ = virConnectOpen(uri.c_str());
    if (!conn_) throw VirtError("cannot connect to '" + uri + "': " + last_error());
}

Connection::~Connection() {
    if (conn_) virConnectClose(conn_);
}

// ---- helpers ------------------------------------------------------------------

std::string to_string(DomState s) {
    switch (s) {
        case DomState::NoState: return "nostate";
        case DomState::Running: return "running";
        case DomState::Blocked: return "blocked";
        case DomState::Paused: return "paused";
        case DomState::ShuttingDown: return "shutting-down";
        case DomState::ShutOff: return "shut-off";
        case DomState::Crashed: return "crashed";
        case DomState::PMSuspended: return "pm-suspended";
        case DomState::Gone: return "gone";
    }
    return "?";
}

std::string build_domain_xml(const std::string& name, unsigned vcpus, unsigned mem_mib,
                             const std::string& disk_path, const std::string& disk_format) {
    std::ostringstream os;
    os << "<domain type='kvm'>\n"
       << "  <name>" << xml_escape(name) << "</name>\n"
       << "  <memory unit='MiB'>" << mem_mib << "</memory>\n"
       << "  <vcpu>" << vcpus << "</vcpu>\n"
       << "  <os>\n"
       << "    <type arch='x86_64' machine='q35'>hvm</type>\n"
       << "    <boot dev='hd'/>\n"
       << "  </os>\n"
       << "  <features><acpi/><apic/></features>\n"
       << "  <cpu mode='host-passthrough'/>\n"
       << "  <clock offset='utc'/>\n"
       << "  <devices>\n"
       << "    <disk type='file' device='disk'>\n"
       << "      <driver name='qemu' type='" << xml_escape(disk_format) << "'/>\n"
       << "      <source file='" << xml_escape(disk_path) << "'/>\n"
       << "      <target dev='vda' bus='virtio'/>\n"
       << "    </disk>\n"
       << "    <serial type='pty'><target port='0'/></serial>\n"
       << "    <console type='pty'><target type='serial' port='0'/></console>\n"
       << "    <channel type='unix'>\n"
       << "      <target type='virtio' name='org.qemu.guest_agent.0'/>\n"
       << "    </channel>\n"
       << "    <memballoon model='virtio'/>\n"
       << "  </devices>\n"
       << "</domain>\n";
    return os.str();
}

// ---- Domain -------------------------------------------------------------------

Domain::~Domain() {
    if (dom_) virDomainFree(dom_);
}

Domain::Domain(Domain&& o) noexcept : dom_(o.dom_), name_(std::move(o.name_)) { o.dom_ = nullptr; }

Domain& Domain::operator=(Domain&& o) noexcept {
    if (this != &o) {
        if (dom_) virDomainFree(dom_);
        dom_ = o.dom_;
        name_ = std::move(o.name_);
        o.dom_ = nullptr;
    }
    return *this;
}

Domain Domain::create_transient(Connection& conn, const std::string& xml, const std::string& name) {
    virDomainPtr d = virDomainCreateXML(conn.get(), xml.c_str(), VIR_DOMAIN_START_AUTODESTROY);
    if (!d) throw VirtError("failed to start domain '" + name + "': " + last_error());
    return Domain(d, name);
}

DomState Domain::state() const {
    if (!dom_) return DomState::Gone;
    int st = 0, reason = 0;
    if (virDomainGetState(dom_, &st, &reason, 0) < 0) {
        virErrorPtr e = virGetLastError();
        if (e && e->code == VIR_ERR_NO_DOMAIN) return DomState::Gone;
        throw VirtError("virDomainGetState failed: " + last_error());
    }
    switch (st) {
        case VIR_DOMAIN_RUNNING: return DomState::Running;
        case VIR_DOMAIN_BLOCKED: return DomState::Blocked;
        case VIR_DOMAIN_PAUSED: return DomState::Paused;
        case VIR_DOMAIN_SHUTDOWN: return DomState::ShuttingDown;
        case VIR_DOMAIN_SHUTOFF: return DomState::ShutOff;
        case VIR_DOMAIN_CRASHED: return DomState::Crashed;
        case VIR_DOMAIN_PMSUSPENDED: return DomState::PMSuspended;
        default: return DomState::NoState;
    }
}

void Domain::suspend() {
    if (virDomainSuspend(dom_) < 0) throw VirtError("suspend failed: " + last_error());
}

void Domain::resume() {
    if (virDomainResume(dom_) < 0) throw VirtError("resume failed: " + last_error());
}

void Domain::shutdown() {
    const unsigned flags = VIR_DOMAIN_SHUTDOWN_GUEST_AGENT | VIR_DOMAIN_SHUTDOWN_ACPI_POWER_BTN;
    if (virDomainShutdownFlags(dom_, flags) < 0) throw VirtError("shutdown failed: " + last_error());
}

void Domain::destroy() {
    if (virDomainDestroy(dom_) < 0) throw VirtError("destroy failed: " + last_error());
}

Domain::Info Domain::info() const {
    virDomainInfo vi;
    if (virDomainGetInfo(dom_, &vi) < 0) throw VirtError("virDomainGetInfo failed: " + last_error());
    Info i;
    i.vcpus = vi.nrVirtCpu;
    i.memory_kib = vi.maxMem;
    return i;
}

std::string Domain::agent_command(const std::string& json, int timeout_s) const {
    char* reply = virDomainQemuAgentCommand(dom_, json.c_str(), timeout_s, 0);
    if (!reply) throw VirtError("guest agent command failed: " + last_error());
    std::string out(reply);
    std::free(reply);
    return out;
}

}  // namespace vmtest
