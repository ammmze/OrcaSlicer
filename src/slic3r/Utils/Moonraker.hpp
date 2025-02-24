#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "NetworkAgent.hpp"
#include "OctoPrint.hpp"
#include <atomic>
#include <thread>

namespace Slic3r {

class MoonrakerMachineObject;

class Moonraker : public OctoPrint
{
public:
    Moonraker(DynamicPrintConfig* config);
    ~Moonraker() override;

    const char* get_name() const override;

    wxString get_test_ok_msg () const override;
    wxString get_test_failed_msg (wxString &msg) const override;
    MachineObject* get_machine(Preset* preset) override;
    std::string query_printer_object_status(std::string printer_object);
};

class MoonrakerMachineObject : public MachineObject {
public:
    MoonrakerMachineObject(Moonraker& print_host, Preset* preset);
    ~MoonrakerMachineObject();
    int connect(bool is_anonymous, bool use_openssl) override;
    int disconnect() override;

private:
    Moonraker m_print_host;
    Preset* m_preset;
    std::atomic<bool> poll_run;
    std::thread poll_thread;
    void refresh_mmu();
    void process_mmu(std::string payload);
};
}