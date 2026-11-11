/*
    Tests for stable serial port names of the serial connection plugin (Linux).

    /dev/ttyUSB<n> numbers change between boots, so a driver that saved /dev/ttyUSB2 can open another device the
    next night. The plugin now lists and saves USB serial ports under names that stay the same: the by-id link for
    an adapter with its own serial number, the by-path link (USB socket) for identical adapters without one (they
    share a single by-id link).

    The naming tests use a fake /dev + /sys tree that mirrors a StellarMate with a CP2102N (mount), an FTDI
    (Pegasus PPBA) and two CH340 adapters without serial numbers (RBF Excalibur, Wanderer ETA).

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <pty.h>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "defaultdevice.h"
#include "indicom.h"
#include "indilogger.h"
#include "connectionplugins/connectionserial.h"
#include "connectionplugins/serialportnames.h"

namespace
{

namespace fs = std::filesystem;
namespace SerialPorts = Connection::SerialPorts;

//////////////////////////////////////////////////////////////////////////////////////////////////
// Fake /dev and /sys tree
//////////////////////////////////////////////////////////////////////////////////////////////////
class FakeSystem
{
    public:
        FakeSystem()
        {
            char pattern[] = "/tmp/test_serial_port_names_XXXXXX";
            const char *dir = mkdtemp(pattern);
            m_Root = dir ? dir : "";
            fs::create_directories(m_Root + "/dev/serial/by-id");
            fs::create_directories(m_Root + "/dev/serial/by-path");
            fs::create_directories(m_Root + "/sys/class/tty");
        }

        ~FakeSystem()
        {
            std::error_code ignored;
            fs::remove_all(m_Root, ignored);
        }

        const std::string &root() const
        {
            return m_Root;
        }

        std::string dev(const std::string &name) const
        {
            return m_Root + "/dev/" + name;
        }

        // A USB serial adapter at USB socket (e.g. "3-3"), as udev and the kernel present it.
        // serial empty = the adapter reports no serial number. byIdName empty = no by-id link points to it.
        void addAdapter(const std::string &tty, const std::string &socket, const std::string &vid, const std::string &pid,
                        const std::string &serial, const std::string &product, const std::string &byIdName)
        {
            const std::string usb = m_Root + "/sys/devices/pci0000:00/0000:00:14.0/usb3/" + socket;
            const std::string interface = usb + "/" + socket + ":1.0";
            fs::create_directories(interface + "/" + tty);
            writeFile(usb + "/idVendor", vid);
            writeFile(usb + "/idProduct", pid);
            writeFile(usb + "/product", product);
            if (!serial.empty())
                writeFile(usb + "/serial", serial);
            writeFile(interface + "/bInterfaceNumber", "00");

            fs::create_directories(m_Root + "/sys/class/tty/" + tty);
            link(interface + "/" + tty, m_Root + "/sys/class/tty/" + tty + "/device");

            writeFile(dev(tty), "");
            if (!byIdName.empty())
                link("../../" + tty, byId(byIdName));
            link("../../" + tty, byPath(socket));
            link("../../" + tty, m_Root + "/dev/serial/by-path/pci-0000:00:14.0-usbv2-0:" + usbPortPath(socket) +
                 ":1.0-port0");
        }

        std::string byPath(const std::string &socket) const
        {
            return m_Root + "/dev/serial/by-path/pci-0000:00:14.0-usb-0:" + usbPortPath(socket) + ":1.0-port0";
        }

        std::string byId(const std::string &name) const
        {
            return m_Root + "/dev/serial/by-id/" + name;
        }

        void link(const std::string &target, const std::string &path) const
        {
            fs::create_symlink(target, path);
        }

    private:
        // "3-1.2.1" -> "1.2.1"
        static std::string usbPortPath(const std::string &socket)
        {
            return socket.substr(socket.find('-') + 1);
        }

        static void writeFile(const std::string &path, const std::string &content)
        {
            std::ofstream out(path);
            out << content << (content.empty() ? "" : "\n");
        }

        std::string m_Root;
};

const char *MOUNT_BY_ID =
    "usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0059b092d833f1118be3945f30d20014-if00-port0";
const char *PPBA_BY_ID = "usb-Pegasus_Astro_PPBADV_Gen2C_PPBA8VSOTK-if00-port0";
const char *CH340_BY_ID = "usb-1a86_USB_Serial-if00-port0";

// The StellarMate of the bug report: two CH340 without serial numbers share one by-id link (pointing to the one
// enumerated last, ttyUSB3).
void addStellarMate(FakeSystem &fake)
{
    fake.addAdapter("ttyUSB0", "3-1.4", "10c4", "ea60", "0059b092d833f1118be3945f30d20014",
                    "CP2102N USB to UART Bridge Controller", MOUNT_BY_ID);
    fake.addAdapter("ttyUSB1", "3-1.1", "0403", "6015", "PPBA8VSOTK", "PPBADV Gen2C", PPBA_BY_ID);
    fake.addAdapter("ttyUSB2", "3-1.2.1", "1a86", "7523", "", "USB Serial", "");
    fake.addAdapter("ttyUSB3", "3-3", "1a86", "7523", "", "USB Serial", CH340_BY_ID);
}

}

TEST(SerialPortNamesTest, uniqueSerialNumberUsesById)
{
    FakeSystem fake;
    addStellarMate(fake);

    EXPECT_EQ(SerialPorts::stableName(fake.dev("ttyUSB0"), fake.root()), fake.byId(MOUNT_BY_ID));
    EXPECT_EQ(SerialPorts::stableName(fake.dev("ttyUSB1"), fake.root()), fake.byId(PPBA_BY_ID));
}

TEST(SerialPortNamesTest, identicalAdaptersWithoutSerialUseUsbSocket)
{
    FakeSystem fake;
    addStellarMate(fake);

    EXPECT_EQ(SerialPorts::stableName(fake.dev("ttyUSB2"), fake.root()), fake.byPath("3-1.2.1"));
    EXPECT_EQ(SerialPorts::stableName(fake.dev("ttyUSB3"), fake.root()), fake.byPath("3-3"));
}

TEST(SerialPortNamesTest, sharedSerialNumberUsesUsbSocket)
{
    // Some cheap adapters all report the same serial number.
    FakeSystem fake;
    fake.addAdapter("ttyUSB0", "3-1", "0403", "6001", "A50285BI", "FT232R USB UART", "");
    fake.addAdapter("ttyUSB1", "3-2", "0403", "6001", "A50285BI", "FT232R USB UART",
                    "usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0");

    EXPECT_EQ(SerialPorts::stableName(fake.dev("ttyUSB0"), fake.root()), fake.byPath("3-1"));
    EXPECT_EQ(SerialPorts::stableName(fake.dev("ttyUSB1"), fake.root()), fake.byPath("3-2"));
}

TEST(SerialPortNamesTest, singleAdapterWithoutSerialUsesUsbSocket)
{
    FakeSystem fake;
    fake.addAdapter("ttyUSB0", "3-3", "1a86", "7523", "", "USB Serial", CH340_BY_ID);

    EXPECT_EQ(SerialPorts::stableName(fake.dev("ttyUSB0"), fake.root()), fake.byPath("3-3"));
}

TEST(SerialPortNamesTest, nameToSaveReplacesRenumberingNames)
{
    FakeSystem fake;
    addStellarMate(fake);

    // Kernel device nodes
    EXPECT_EQ(SerialPorts::nameToSave(fake.dev("ttyUSB0"), fake.root()), fake.byId(MOUNT_BY_ID));
    EXPECT_EQ(SerialPorts::nameToSave(fake.dev("ttyUSB2"), fake.root()), fake.byPath("3-1.2.1"));
    // The by-id link shared by the two CH340 points to whichever was enumerated last.
    EXPECT_EQ(SerialPorts::nameToSave(fake.byId(CH340_BY_ID), fake.root()), fake.byPath("3-3"));
}

TEST(SerialPortNamesTest, nameToSaveKeepsStableAndCustomNames)
{
    FakeSystem fake;
    addStellarMate(fake);
    fake.link("ttyUSB3", fake.dev("ttyUSB_excalibur"));
    fake.link("ttyUSB0", fake.dev("mount"));

    for (const std::string &port :
            {
                fake.byId(MOUNT_BY_ID), fake.byPath("3-3"), fake.dev("ttyUSB_excalibur"), fake.dev("mount"),
                fake.dev("rfcomm0"), fake.dev("ttyUSB9")
            })
        EXPECT_EQ(SerialPorts::nameToSave(port, fake.root()), port);
}

TEST(SerialPortNamesTest, listShowsStableNames)
{
    FakeSystem fake;
    addStellarMate(fake);
    // Custom udev names are not listed twice.
    fake.link("ttyUSB3", fake.dev("ttyUSB_excalibur"));

    const std::vector<std::string> expected =
    {
        fake.byId(MOUNT_BY_ID), fake.byId(PPBA_BY_ID), fake.byPath("3-1.2.1"), fake.byPath("3-3")
    };
    EXPECT_EQ(SerialPorts::list(fake.root()), expected);
}

TEST(SerialPortNamesTest, labelShowsProductAndSocket)
{
    FakeSystem fake;
    addStellarMate(fake);

    EXPECT_EQ(SerialPorts::label(fake.byPath("3-1.2.1"), fake.root()), "USB Serial @ USB 3-1.2.1");
    EXPECT_EQ(SerialPorts::label(fake.byId(MOUNT_BY_ID), fake.root()), "");
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
