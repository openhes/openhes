# Standalone SensorTag CC2650 hardware programs.
#
# Opt-in via the root CMakeLists' OPENHES_BUILD_DEVICE_TESTS. These are NOT
# ctest tests and NOT part of `make all`: each is a self-contained BlueZ D-Bus
# client with its own main() that connects to a real SensorTag, reads one
# characteristic and prints it. Running one needs a Bluetooth adapter, a
# paired/trusted tag and a running BlueZ -- none of which exists in the build
# container -- so they are built here and run on the host.
#
# Note they do NOT link the accessors in the parent directory: those are the
# extracted, library-style versions of the same reads, and these programs
# predate them. Only GLib/GIO is needed, for the D-Bus calls.
#
# They are also exempt from the project's -Wall policy: they are throwaway
# bring-up programs, kept verbatim as the reference for each sensor's real GATT
# UUIDs, enable byte and conversion (which the parent directory's accessors
# mirror and document).

set(SENSORTAG_DEVICE_TESTS
  sensortag_bar_test
  sensortag_battery_test
  sensortag_button_test
  sensortag_humid_test
  sensortag_light_test
  sensortag_mic_test
  sensortag_motion_test
)

foreach(_test ${SENSORTAG_DEVICE_TESTS})
  add_executable(${_test} ${_test}.c)
  target_link_libraries(${_test} PRIVATE PkgConfig::GLIB2)
endforeach()

unset(_test)
