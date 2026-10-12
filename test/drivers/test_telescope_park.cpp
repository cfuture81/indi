/*
    Regression test for INDI::Telescope park handling: a second TELESCOPE_PARK request while a park is
    in progress cancels the park. The cancel must leave the driver in the state it had before parking,
    exactly like TELESCOPE_ABORT_MOTION does, and not stuck in SCOPE_PARKING.

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "indilogger.h"
#include "inditelescope.h"

char _me[] = "ParkCancelScope";
char *me = _me;

// Minimal telescope: Park() starts a park that never finishes on its own, Abort() stops motion without
// touching TrackState (like LX200Telescope::Abort(), used by the ZWO AM5 driver).
class ParkCancelScope : public INDI::Telescope
{
    public:
        ParkCancelScope()
        {
            SetTelescopeCapability(TELESCOPE_CAN_PARK | TELESCOPE_CAN_ABORT, 0);
            initProperties();
            TrackState = SCOPE_TRACKING;
        }

        const char *getDefaultName() override
        {
            return "Park Cancel Test";
        }

        bool ReadScopeStatus() override
        {
            return true;
        }

        bool Park() override
        {
            parkCalls++;
            TrackState = SCOPE_PARKING;
            return true;
        }

        bool Abort() override
        {
            abortCalls++;
            return true;
        }

        // Send TELESCOPE_PARK PARK=On through the regular client path.
        void requestPark()
        {
            ISState states[] = { ISS_ON, ISS_OFF };
            char park[] = "PARK", unpark[] = "UNPARK";
            char *names[] = { park, unpark };
            ASSERT_TRUE(ISNewSwitch(getDeviceName(), "TELESCOPE_PARK", states, names, 2));
        }

        TelescopeStatus trackState() const
        {
            return TrackState;
        }

        IPState parkState() const
        {
            return ParkSP.getState();
        }

        int parkCalls {0};
        int abortCalls {0};
};

TEST(TelescopeParkTest, cancelParkRestoresTrackState)
{
    ParkCancelScope scope;
    ASSERT_EQ(scope.trackState(), INDI::Telescope::SCOPE_TRACKING);

    // First request: the park starts.
    scope.requestPark();
    EXPECT_EQ(scope.parkCalls, 1);
    EXPECT_EQ(scope.abortCalls, 0);
    EXPECT_EQ(scope.trackState(), INDI::Telescope::SCOPE_PARKING);
    EXPECT_EQ(scope.parkState(), IPS_BUSY);

    // Second request while parking: the park is cancelled (toggle), the mount is aborted.
    scope.requestPark();
    EXPECT_EQ(scope.parkCalls, 1);
    EXPECT_EQ(scope.abortCalls, 1);
    EXPECT_EQ(scope.parkState(), IPS_ALERT);
    // The cancelled park must not leave the driver in SCOPE_PARKING: a driver whose ReadScopeStatus() treats
    // SCOPE_PARKING as "park in progress" would otherwise declare the mount parked wherever it stopped.
    EXPECT_EQ(scope.trackState(), INDI::Telescope::SCOPE_TRACKING);
}

int main(int argc, char **argv)
{
    // Never read or write the user's real ~/.indi driver configuration.
    char home[] = "/tmp/test_telescope_park_XXXXXX";
    if (mkdtemp(home) == nullptr)
        return 1;
    setenv("HOME", home, 1);
    setenv("INDICONFIG", (std::string(home) + "/config.xml").c_str(), 1);
    INDI::Logger::getInstance().configure("", INDI::Logger::file_off,
                                          INDI::Logger::DBG_ERROR, INDI::Logger::DBG_ERROR);
    ::testing::InitGoogleTest(&argc, argv);
    int rc = RUN_ALL_TESTS();
    std::filesystem::remove_all(home);
    return rc;
}
