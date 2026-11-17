/*
 * The "Fix mapping" dialog (T13, design section 5): pick which of this machine's devices each of a show profile's
 * device slots uses. A choice applies to the live source at once and is remembered for this machine.
 */

#pragma once

class QWidget;

/* Opens (or raises and reloads) the dialog for show profile showId. */
void ctd_device_map_dialog_open(QWidget *parent, const char *showId);

/* Registers the dialog as the opener show_devices calls after a switch; parent is the main window or dock. */
void ctd_device_map_dialog_install(QWidget *parent);
