/**
 *  File:        tmux_bluetooth.c
 *  Author:      mohamadrnasr
 *  Created:     Oct 20, 2026
 *  Last change: Oct 20, 2026
 *  Description: tmux bluetooth status
 **/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define BLUEZ_SERVICE "org.bluez"
#define OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define DEVICE_INTERFACE "org.bluez.Device1"

static int connected_count = 0;

/*
 * Count currently connected Bluetooth devices.
 */
static int count_connected(sd_bus *bus) {
  sd_bus_message *reply = NULL;
  sd_bus_error error = SD_BUS_ERROR_NULL;
  int r;
  int count = 0;

  r = sd_bus_call_method(bus, BLUEZ_SERVICE, "/", OBJECT_MANAGER,
                         "GetManagedObjects", &error, &reply, "");

  if (r < 0) {
    fprintf(stderr, "GetManagedObjects failed: %s\n", error.message);
    sd_bus_error_free(&error);
    return -1;
  }

  r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
  if (r < 0)
    goto finish;

  while ((r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
    const char *path;

    r = sd_bus_message_read(reply, "o", &path);
    if (r < 0)
      break;

    r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
    if (r < 0)
      break;

    while ((r = sd_bus_message_enter_container(reply, 'e', "sa{sv}")) > 0) {
      const char *interface;

      r = sd_bus_message_read(reply, "s", &interface);
      if (r < 0)
        break;

      if (strcmp(interface, DEVICE_INTERFACE) == 0) {
        int connected = 0;

        r = sd_bus_message_enter_container(reply, 'a', "{sv}");
        if (r < 0)
          break;

        while ((r = sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
          const char *property;
          char type;

          r = sd_bus_message_read(reply, "s", &property);
          if (r < 0)
            break;

          r = sd_bus_message_peek_type(reply, &type, NULL);
          if (r < 0)
            break;

          if (strcmp(property, "Connected") == 0) {
            r = sd_bus_message_enter_container(reply, 'v', "b");
            if (r < 0)
              break;

            r = sd_bus_message_read(reply, "b", &connected);
            sd_bus_message_exit_container(reply);

            if (r < 0)
              break;
          } else {
            r = sd_bus_message_skip(reply, "v");
            if (r < 0)
              break;
          }

          r = sd_bus_message_exit_container(reply);
          if (r < 0)
            break;
        }

        sd_bus_message_exit_container(reply);

        if (connected)
          count++;
      } else {
        r = sd_bus_message_skip(reply, "a{sv}");
        if (r < 0)
          break;
      }

      r = sd_bus_message_exit_container(reply);
      if (r < 0)
        break;
    }

    sd_bus_message_exit_container(reply);

    r = sd_bus_message_exit_container(reply);
    if (r < 0)
      break;
  }

  sd_bus_message_exit_container(reply);

finish:
  sd_bus_error_free(&error);
  sd_bus_message_unref(reply);

  return count;
}

/*
 * Print the status expected by tmux.
 */
static void print_status(int count) {
  if (count > 0)
    printf("𑩂 %d\n", count);
  else
    printf("\n");

  fflush(stdout);
}

/*
 * Called whenever BlueZ emits PropertiesChanged.
 */
static int properties_changed(sd_bus_message *m, void *userdata,
                              sd_bus_error *ret_error) {
  (void)ret_error;

  sd_bus *bus = userdata;

  const char *interface;
  int r;

  r = sd_bus_message_read(m, "s", &interface);
  if (r < 0)
    return r;

  /*
   * We only care about Device1.
   */
  if (strcmp(interface, DEVICE_INTERFACE) != 0)
    return 0;

  /*
   * Read changed properties.
   */
  r = sd_bus_message_enter_container(m, 'a', "{sv}");
  if (r < 0)
    return r;

  while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
    const char *property;

    r = sd_bus_message_read(m, "s", &property);
    if (r < 0)
      break;

    if (strcmp(property, "Connected") == 0) {
      int connected;

      r = sd_bus_message_enter_container(m, 'v', "b");
      if (r < 0)
        break;

      r = sd_bus_message_read(m, "b", &connected);

      sd_bus_message_exit_container(m);

      if (r < 0)
        break;

      /*
       * The value itself isn't enough if we are tracking
       * multiple devices, so recalculate the total.
       */
      int new_count = count_connected(bus);

      if (new_count >= 0 && new_count != connected_count) {
        connected_count = new_count;
        print_status(connected_count);
      }
    } else {
      r = sd_bus_message_skip(m, "v");
      if (r < 0)
        break;
    }

    sd_bus_message_exit_container(m);
  }

  sd_bus_message_exit_container(m);

  return 0;
}

int main(void) {
  sd_bus *bus = NULL;
  sd_bus_slot *slot = NULL;
  int r;

  /*
   * Connect to the system D-Bus.
   */
  r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
    return EXIT_FAILURE;
  }


  /*
   * Subscribe to Device1 PropertiesChanged signals.
   */
  r = sd_bus_match_signal(bus, &slot, BLUEZ_SERVICE, NULL, PROPERTIES,
                          "PropertiesChanged", properties_changed, bus);

  if (r < 0) {
    fprintf(stderr, "Failed to subscribe to PropertiesChanged: %s\n",
            strerror(-r));
    goto finish;
  }

  /*
   * Get initial state.
   */
  connected_count = count_connected(bus);

  if (connected_count < 0)
    connected_count = 0;

  print_status(connected_count);

  /*
   * Main event loop.
   *
   * sd_bus_process() returns immediately if there is nothing to process.
   * sd_bus_wait() then blocks until D-Bus has something for us.
   */
  for (;;) {
    r = sd_bus_process(bus, NULL);

    if (r < 0) {
      fprintf(stderr, "D-Bus processing failed: %s\n", strerror(-r));
      break;
    }

    if (r > 0)
      continue;

    r = sd_bus_wait(bus, UINT64_MAX);

    if (r < 0) {
      fprintf(stderr, "D-Bus wait failed: %s\n", strerror(-r));
      break;
    }
  }

finish:
  sd_bus_slot_unref(slot);
  sd_bus_unref(bus);

  return EXIT_FAILURE;
}
