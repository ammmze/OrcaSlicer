#include "Http.hpp"
#include "libslic3r/Preset.hpp"
#include "Moonraker.hpp"
#include "NetworkAgent.hpp"
#include "OctoPrint.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cmath>
#include <exception>
#include <sstream>
#include <thread>

namespace pt = boost::property_tree;

namespace Slic3r {

Moonraker::Moonraker(DynamicPrintConfig* config)
    : OctoPrint(config)
{
    std::cout << __FUNCTION__ << " creating moonraker print host" << std::endl;
}

Moonraker::~Moonraker()
{
    std::cout << __FUNCTION__ << " destroying moonraker print host" << std::endl;
    DeviceManager* dev = GUI::wxGetApp().getDeviceManager();
    // auto dev_id = GUI::wxGetApp().preset_bundle->printers.get_edited_preset().label(false);
    // dev->localMachineList[dev_id]->disconnect();
    // if (machine != nullptr) {
    //     machine->disconnect();
    // }
}

const char* Moonraker::get_name() const { return "Moonraker"; }

wxString Moonraker::get_test_ok_msg() const
{
    return _(L("Connection to Moonraker works correctly."));
}

wxString Moonraker::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Moonraker"), msg);
}

MachineObject* Moonraker::get_machine(Preset* preset)
{
    DeviceManager* dev = GUI::wxGetApp().getDeviceManager();
    std::cout << __FUNCTION__ << boost::format(" before selected machine %1%")%dev->selected_machine << std::endl;

    auto preset_bundle = GUI::wxGetApp().preset_bundle;
    auto p_preset = preset_bundle->printers.get_edited_preset();
    p_preset.get_printer_type(preset_bundle);

    // machine = new MachineObject(nil "my printer", "klippertmp", m_host)
    MoonrakerMachineObject* machine;
    machine = new MoonrakerMachineObject(*this, &p_preset);
    // dev->localMachineList[machine->dev_id] = machine;
    // machine->connect(false, true);
    // dev->set_selected_machine(machine->dev_id);
    std::cout << __FUNCTION__ << boost::format(" moonraker machine available? %1%")%machine->is_avaliable() << std::endl;
    std::cout << __FUNCTION__ << boost::format(" after selected machine %1%")%dev->selected_machine << std::endl;
    // BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("machine list %1%")%dev->get_my_machine_list();
    return machine;
}

std::string Moonraker::query_printer_object_status(std::string printer_object)
{
    auto url = make_url("printer/objects/query?" + printer_object); // TODO: url encoding
    auto http = Http::get(url);
    set_auth(http);
    std::string msg;
    http.on_complete([&, this](std::string body, unsigned /* http_status */) {
        msg = body;
        // process_mmu(body);
    }).perform_sync();
    return msg;
}

MoonrakerMachineObject::MoonrakerMachineObject(Moonraker& print_host, Preset* preset)
: MachineObject(nullptr, preset->label(false), preset->label(false), "127.0.0.1")
, m_print_host{print_host}
{
    m_preset = preset;
    bind_state = "free";
    dev_connection_type = "lan";
    ams_exist_bits = 1;
    printer_type = m_preset->get_printer_type(GUI::wxGetApp().preset_bundle);
    sdcard_state = MachineObject::SdcardState::HAS_SDCARD_NORMAL;
    set_access_code("noop");
}

int MoonrakerMachineObject::connect(bool is_anonymous, bool use_openssl)
{
    if (poll_run) {
        std::cout << __FUNCTION__ << boost::format(" moonraker connect ... poll running ") << this << std::endl;
        return -1;
    }
    std::cout << __FUNCTION__ << boost::format(" moonraker connect ") << this << std::endl;
    
    // TODO: background processing...polling? websocket?
    poll_run = true;
    this->poll_thread = std::thread([this]() mutable {
        try {
            while (poll_run) {// running
                // Do your polling work here
                std::cout << "Polling..." << this << " " << dev_name << std::endl;
                refresh_mmu();

                // Sleep for a specified duration
                std::this_thread::sleep_for(std::chrono::milliseconds(5000)); 
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception caught: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Exception caught: " << std::endl;
        }
    });
    return -1;
}

int MoonrakerMachineObject::disconnect()
{
    std::cout << __FUNCTION__ << " disconnecting" << std::endl;
    if (poll_thread.joinable()) {
        poll_run = false;
        poll_thread.join();
    }
    return -1;
}

void MoonrakerMachineObject::refresh_mmu()
{
    process_mmu(m_print_host.query_printer_object_status("mmu"));
}

void MoonrakerMachineObject::process_mmu(std::string payload)
{
    // std::cout << __FUNCTION__ << boost::format(" processing mmu %1%")%payload << std::endl;

    try {
        json j = json::parse(payload);
        std::stringstream ss(payload);
        pt::ptree ptree;
        pt::read_json(ss, ptree);
        
        // just check to see if we have one of the mmu attributes
        if (!j.contains(json::json_pointer("/result/status/mmu/enabled"))) {
            return;
        }

        const auto mmu = j.at(json::json_pointer("/result/status/mmu"));

        /* Used to split the gates into "virtual" AMS units with up to 4 gates each */
        const auto virtual_ams_size = 4;
        const auto num_gates = mmu.value("num_gates", 0);
        std::cout << "Found mmu with " << num_gates << " gates" << std::endl;

        std::map<std::string, Ams*> m_ams_list;

        for (int gate_id = 0; gate_id < num_gates; gate_id++) {
            const auto ams_id = std::to_string((int) std::floor(gate_id / virtual_ams_size));
            
            Ams* ams = m_ams_list[ams_id];
            if (ams == nullptr) {
                ams = new Ams(ams_id);
                m_ams_list[ams_id] = ams;
                ams->is_exists = true;
            }

            const auto tray_id = std::to_string(gate_id % virtual_ams_size);
            AmsTray* tray = new AmsTray(tray_id);
            ams->trayList.insert(std::make_pair(tray_id, tray));

            tray->is_exists = mmu.at("gate_status")[gate_id] > 0;
            tray->color = mmu.at("gate_color")[gate_id];
            if (tray->color.size() == 6) {
                tray->color = tray->color + "FF";
            }
            // TODO: Figure out base material type (e.g. PLA+ should just be PLA)
            tray->type = mmu.at("gate_material")[gate_id];

            // TODO: other material attributes
            // tray0->is_exists = true;
            // tray0->color = "000000FF";
            // tray0->type = "PLA";
            // tray0->diameter = 1.75;
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << " mmu to ams_mapping ams id = " << ams->id << ", tray_id = " << tray->id << ", color = " << tray->color << ", type = " << tray->type;
        }

        amsList = m_ams_list;

        module_vers.clear();
        ModuleVersionInfo ver_info;
        ver_info.name = "dummy";
        module_vers.emplace(ver_info.name, ver_info);
        m_push_count++;
        last_push_time = std::chrono::system_clock::now();

        // const auto text = ptree.get_optional<std::string>("text");
        // res = validate_version_text(text);
        // if (! res) {
        //     msg = GUI::from_u8((boost::format(_utf8(L("Mismatched type of print host: %s"))) % (text ? *text : "AstroBox")).str());
        // }
    }
    catch (const std::exception &) {
        // res = false;
        // msg = "Could not parse server response";
    }
}

MoonrakerMachineObject::~MoonrakerMachineObject()
{
    std::cout << __FUNCTION__ << boost::format("closing moonraker machine object") << std::endl;
    disconnect();
}

}