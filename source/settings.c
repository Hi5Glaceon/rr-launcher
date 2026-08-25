/*
    settings.c - the settings menu implementation when auto-launch is interrupted

    Copyright (C) 2025  Retro Rewind Team

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "shutdown.h"
#include "settings.h"
#include "util.h"
#include "console.h"
#include "settingsfile.h"
#include "update/update.h"
#include "prompt.h"
#include <riivo.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <errno.h>
#include <mxml.h>
#include <gccore.h>
#include "pad.h"
#include "nat_test.h"
#include "private_port.h"
#include "upnp.h"
#include "server_status.h"

enum settings_entry_type
{
    ENTRY_TYPE_SELECT,
    ENTRY_TYPE_BUTTON
};

struct settings_entry
{
    /** The type of entry. */
    enum settings_entry_type type;

    /** Any additional newlines to print above (used to divide sections, i.e. having "Launch" and "Exit" separated by two lines) */
    u8 margin_top;

    /**
     * The initial selected option after the saved settings were loaded or saved again. Used for checking whether we have unsaved changes at any point.
     */
    u32 initial_selected_option;

    /**
     * A pointer that stores the option that is currently selected (index into `options`), iff this is a select option (otherwise NULL).
     * This is typically a pointer into a settingsfile struct field so that it automatically gets synchronized.
     */
    u32 *selected_option;

    /** The label or "name" of this setting to be displayed. */
    char *label;

    /** An array of possible selectable option names, if this is a select option (otherwise NULL). */
    const char **options;

    /** Number of selectable options. Must not be 0 if this is a selectable. */
    int option_count;
};

static char *launch_label = "Launch Game";

static char *save_label = "Save changes";
static char *changes_saved_status = RRC_CON_ANSI_FG_BRIGHT_GREEN "Changes saved." RRC_CON_ANSI_CLR;
static char *changes_not_saved_status = RRC_CON_ANSI_BG_BRIGHT_RED "Error saving changes." RRC_CON_ANSI_CLR;

static char *my_stuff_label = "My Stuff";
static char *savegame_label = "Separate savegame";
static char *autoupdate_label = "Automatic updates";

static char *perform_updates_label = "Perform updates";

static char *manage_channel_installation_label = "Manage channel installation";

static char *extras_label = "Extras";

static char *exit_label = "Exit Channel";

static char *cursor_icon = ">> ";

static struct rrc_result xml_find_option_choices(mxml_node_t *node, mxml_node_t *top, const char *name, const char ***result_choice, int *result_choice_count, u32 *saved_value)
{
    mxml_node_t *option_node = mxmlFindElement(node, top, "option", "name", name, MXML_DESCEND);
    if (option_node == NULL)
    {
        return rrc_result_create_error_corrupted_rr_xml("missing option in xml");
    }

    mxml_index_t *index = mxmlIndexNew(option_node, "choice", "name");
    if (index == NULL)
    {
        return rrc_result_create_error_corrupted_rr_xml("failed to create index");
    }

    int count = mxmlIndexGetCount(index);
    const char **out = malloc(sizeof(char *) * (count + 1 /* + implicit 'disabled' */));
    mxmlIndexDelete(index);
    out[0] = "Disabled";

    int i = 1;
    // NOTE: the element order is important here as the settings uses indices, and mxml index sorts them by name, so we can't use the index here.
    for (mxml_node_t *choice = mxmlFindElement(option_node, top, "choice", NULL, NULL, MXML_DESCEND_FIRST); choice != NULL; choice = mxmlGetNextSibling(choice))
    {
        if (mxmlGetType(choice) != MXML_ELEMENT)
        {
            continue;
        }

        const char *choice_s = mxmlElementGetAttr(choice, "name");
        if (choice_s == NULL)
        {
            return rrc_result_create_error_corrupted_rr_xml("choice has no name attribute");
        }
        else if (i >= count + 1)
        {
            return rrc_result_create_error_corrupted_rr_xml("index has more elements than choices");
        }
        out[i++] = choice_s;
    }

    *result_choice = out;
    *result_choice_count = i;

    // Clamp it in case the saved version is out of bounds.
    if (*saved_value < 0 || *saved_value >= *result_choice_count)
    {
        *saved_value = RRC_SETTINGSFILE_DEFAULT;
    }

    return rrc_result_success;
}

static bool prompt_save_unsaved_changes(void *xfb, const struct settings_entry *entries, int entry_count)
{
    char *lines[] = {
        "There are unsaved changes.\n",
        "Would you like to save before exiting settings?"};

    enum rrc_prompt_result result = rrc_prompt_yes_no(xfb, lines, sizeof(lines) / sizeof(char *));
    return result == RRC_PROMPT_RESULT_YES;
}

/*
 * Blocks until the user presses HOME, A, or B, while still servicing
 * shutdown requests. Used to let the user dismiss a test-result screen
 * (UPnP Status / Ping Test / NAT Test) that stays on screen until
 * acknowledged. Previously this loop was copy-pasted at all three call
 * sites; now there is a single place to change if the dismiss buttons
 * ever need to change.
 */
static void rrc_wait_for_result_dismiss(void)
{
    while (1)
    {
        struct pad_state result_pad = rrc_pad_buttons();

        if (rrc_pad_home_pressed(result_pad) ||
            rrc_pad_a_pressed(result_pad) ||
            rrc_pad_b_pressed(result_pad))
            break;

        rrc_shutdown_check();
        usleep(RRC_WPAD_LOOP_TIMEOUT);
    }
}

#define CLEANUP             \
    free(my_stuff_options); \
    free(savegame_options); \
    mxmlDelete(xml_top);    \
    fclose(xml_file);


static void rrc_extras_display(void *xfb)
{
    const char *entries[] = {
        "Perform NAT Test (Experimental; may take up to 50 seconds)",
        "Ping Test",
        "UPnP Status",
        "Server Status",
        "Back"
    };
    const int entry_count = sizeof(entries) / sizeof(entries[0]);
    int selected_idx = 0;

    while (1)
    {
        rrc_shutdown_check();
        rrc_con_clear(true);

        for (int row = 0; row < rrc_con_get_rows(); row++)
            rrc_con_clear_line(row);

        rrc_con_cursor_seek_to(0, 0);

        printf("Extras\n\n");

        for (int i = 0; i < entry_count; i++)
        {
            if (i == selected_idx)
            {
                printf(RRC_CON_ANSI_FG_BRIGHT_WHITE);
                printf(cursor_icon);
            }
            else
            {
                for (size_t j = 0; j < strlen(cursor_icon); j++)
                    putc(' ', stdout);
            }

            if (i == 3 && rr_server_status_has_alert())
            {
            printf("%sServer Status (1)%s",
            rr_server_status_alert_color(),
            RRC_CON_ANSI_CLR);
            }
            else
            {
            printf("%s", entries[i]);
            }

printf("\n");
        }

        printf("\n");
        rrc_con_cursor_seek_to(rrc_con_get_rows() - 2, strlen(cursor_icon));
        printf("Use the D-Pad to navigate.");

        while (1)
        {
            struct pad_state pad = rrc_pad_buttons();

            if (rrc_pad_home_pressed(pad) || rrc_pad_b_pressed(pad))
                return;

            if (rrc_pad_down_pressed(pad))
            {
                selected_idx = (selected_idx + 1) % entry_count;
                break;
            }

            if (rrc_pad_up_pressed(pad))
            {
                selected_idx = (selected_idx + entry_count - 1) % entry_count;
                break;
            }

            if (rrc_pad_a_pressed(pad))
            {
                if (selected_idx == 4)
                    return;

                if (selected_idx == 3)
                {
                    rr_server_status_test();
                    break;
                }

                if (selected_idx == 2)
                {
                    uint16_t private_port = 0;
                    bool upnp_ok =
                        rr_upnp_prepare_mkwii_mapping(&private_port);

                    rrc_con_clear(true);

                    for (int row = 0;
                         row < rrc_con_get_rows();
                         row++)
                    {
                        rrc_con_clear_line(row);
                    }

                    rrc_con_cursor_seek_to(0, 26);

                    printf("UPnP Status\n\n");

                    if (upnp_ok)
                    {
                        printf("Private Port: %u\n", (unsigned)private_port);
                    }
                    else
                    {
                        printf("Private Port: unavailable\n");
                        printf("Note: Private port might be unavailable because UPnP failed.\n");
                    }

                    printf("UPnP: %s%s%s\n",
                           upnp_ok ? RRC_CON_ANSI_FG_BRIGHT_GREEN : RRC_CON_ANSI_FG_BRIGHT_RED,
                           upnp_ok ? "SUCCESS" : "FAILED",
                           RRC_CON_ANSI_CLR);

                    if (!upnp_ok)
                    {
                        const rr_upnp_error_info_t *upnp_error = rr_upnp_last_error();
                        int wrap_width = rrc_con_get_cols() - (RRC_CON_EDGE_PAD * 2);

                        printf("\n");

                        if (upnp_error && upnp_error->code > 0)
                        {
                            printf("Error Code: %d\n", upnp_error->code);
                            printf("Error: %s%s%s\n",
                                   RRC_CON_ANSI_FG_BRIGHT_YELLOW,
                                   upnp_error->name,
                                   RRC_CON_ANSI_CLR);
                            printf("\n");
                            rrc_con_print_wrapped(upnp_error->description, wrap_width);
                            printf("\nSolution:\n");
                            rrc_con_print_wrapped(upnp_error->solution, wrap_width);
                        }
                        else
                        {
                            printf("No router error code was returned.\n");

                            if (upnp_error && upnp_error->description[0])
                                rrc_con_print_wrapped(upnp_error->description, wrap_width);

                            if (upnp_error && upnp_error->solution[0])
                            {
                                printf("\nSolution:\n");
                                rrc_con_print_wrapped(upnp_error->solution, wrap_width);
                            }
                        }
                    }

                    rrc_wait_for_result_dismiss();

                    break;
                }

                if (selected_idx == 1)
                {
                    rr_ping_result_t ping_result;
                    bool test_ok = rr_ping_test_run(&ping_result);

                    rrc_con_clear(true);

                    for (int row = 0; row < rrc_con_get_rows(); row++)
                        rrc_con_clear_line(row);

                    rrc_con_cursor_seek_to(0, 28);
                    printf("Ping Test\n\n");

                    if (!test_ok)
                    {
                        printf("STUN Anycast (Google)\n");
                        printf("No response received.\n");
                    }
                    else
                    {
                        printf("STUN Anycast (Google)\n");
                        printf("stun-anycast.l.google.com:19302\n\n");
                        printf("  Min              %u ms\n", ping_result.min_ms);
                        printf("  Average          %u ms\n", ping_result.avg_ms);
                        printf("  Max              %u ms\n", ping_result.max_ms);
                    }

                    rrc_wait_for_result_dismiss();

                    break;
                }

                rr_nat_result_t nat_result;
                bool test_ok = rr_nat_test_run(&nat_result);

                rrc_con_clear(true);

                for (int row = 0; row < rrc_con_get_rows(); row++)
                    rrc_con_clear_line(row);

                rrc_con_cursor_seek_to(0, 0);

                if (!test_ok)
                {
                    char *lines[] = {
                        "NAT Test",
                        "",
                        "The NAT test could not be completed."
                    };
                    rrc_prompt_1_option(xfb, lines, 3, "OK");
                    break;
                }

                char ip_server2[16] = "Unavailable";
                char ip_server1[16] = "Unavailable";

                if (nat_result.stun_server2)
                {
                    snprintf(ip_server2, sizeof(ip_server2),
                             "%u.%u.%u.%u",
                             (unsigned)((nat_result.public_ip_server2 >> 24) & 0xff),
                             (unsigned)((nat_result.public_ip_server2 >> 16) & 0xff),
                             (unsigned)((nat_result.public_ip_server2 >> 8) & 0xff),
                             (unsigned)(nat_result.public_ip_server2 & 0xff));
                }

                if (nat_result.stun_server1)
                {
                    snprintf(ip_server1, sizeof(ip_server1),
                             "%u.%u.%u.%u",
                             (unsigned)((nat_result.public_ip_server1 >> 24) & 0xff),
                             (unsigned)((nat_result.public_ip_server1 >> 16) & 0xff),
                             (unsigned)((nat_result.public_ip_server1 >> 8) & 0xff),
                             (unsigned)(nat_result.public_ip_server1 & 0xff));
                }

                rrc_con_cursor_seek_to(1, 25);
                printf("Perform NAT Test\n\n");
                uint16_t private_port = 0;
                bool private_port_ok =
                    rr_get_mkwii_private_port(&private_port);
                if (private_port_ok)
                printf("Using local UDP port: %u\n",
                (unsigned)private_port);
                else
                printf("Using local UDP port: FAILED\n");
                
                printf("%-17s%s%s%s\n",
                       "STUN Telekom",
                       nat_result.stun_server1 ? RRC_CON_ANSI_FG_BRIGHT_GREEN : RRC_CON_ANSI_FG_BRIGHT_RED,
                       nat_result.stun_server1 ? "PASS" : "FAIL",
                       RRC_CON_ANSI_CLR);

            if (nat_result.stun_server1)
            {
                printf("  Public IP:   %s\n", ip_server1);
                printf("  Public Port: %u\n",
                       nat_result.public_port_server1);
            }

            printf("\n");

            printf("%-17s%s%s%s\n",
                   "STUN Cloudflare",
                   nat_result.stun_server2 ? RRC_CON_ANSI_FG_BRIGHT_GREEN
                                           : RRC_CON_ANSI_FG_BRIGHT_RED,
                   nat_result.stun_server2 ? "PASS" : "FAILED",
                   RRC_CON_ANSI_CLR);

            if (nat_result.stun_server2)
            {
                printf("  Public IP:   %s\n", ip_server2);
                printf("  Public Port: %u\n",
                       nat_result.public_port_server2);
            }

            printf("\n");

printf("%-17s%s%s%s\n",
       "NAT Test",
       (nat_result.stun_server2 && nat_result.stun_server1)
           ? RRC_CON_ANSI_FG_BRIGHT_GREEN
           : RRC_CON_ANSI_FG_BRIGHT_RED,
       (nat_result.stun_server2 && nat_result.stun_server1)
           ? "SUCCESS"
           : "FAILED",
       RRC_CON_ANSI_CLR);

            printf("  %-19s", "NAT Type");

            switch (nat_result.nat_type)
            {
                case RR_NAT_ENDPOINT_INDEPENDENT:
                    if (nat_result.nat_filter_type ==
                        RR_NAT_FILTER_ENDPOINT_INDEPENDENT)
                        printf("Full Cone\n");
                    else if (nat_result.nat_filter_type ==
                             RR_NAT_FILTER_ADDRESS_DEPENDENT)
                        printf("Restricted Cone\n");
                    else if (nat_result.nat_filter_type ==
                             RR_NAT_FILTER_ADDRESS_PORT_DEPENDENT)
                        printf("Port Restricted Cone\n");
                    else
                        printf("Endpoint-Independent\n");
                    break;

                case RR_NAT_SYMMETRIC:
                    printf("Symmetric\n");
                    break;

                default:
                    printf("Unknown\n");
                    break;
            }

            printf("  %-19s", "NAT Mapping");

            switch (nat_result.nat_type)
            {
                case RR_NAT_ENDPOINT_INDEPENDENT:
                    printf("Endpoint-Independent\n");
                    break;

                case RR_NAT_SYMMETRIC:
                    printf("Symmetric\n");
                    break;

                default:
                    printf("Unknown\n");
                    break;
            }

            printf("  %-19s", "NAT Filtering");

            switch (nat_result.nat_filter_type)
            {
                case RR_NAT_FILTER_ENDPOINT_INDEPENDENT:
                    printf("Endpoint-Independent\n");
                    break;

                case RR_NAT_FILTER_ADDRESS_DEPENDENT:
                    printf("Address-Dependent\n");
                    break;

                case RR_NAT_FILTER_ADDRESS_PORT_DEPENDENT:
                    printf("Port-Restricted\n");
                    break;

                default:
                    printf("Unknown\n");
                    break;
            }

printf("  %-19s%s%s%s\n",
       "UDP outbound",
       nat_result.udp_outbound ? RRC_CON_ANSI_FG_BRIGHT_GREEN : RRC_CON_ANSI_FG_BRIGHT_RED,
       nat_result.udp_outbound ? "PASS" : "FAIL",
       RRC_CON_ANSI_CLR);

printf("  %-19s%s%s%s\n",
       "UDP inbound",
       nat_result.udp_inbound ? RRC_CON_ANSI_FG_BRIGHT_GREEN : RRC_CON_ANSI_FG_BRIGHT_RED,
       nat_result.udp_inbound ? "PASS" : "UNKNOWN",
       RRC_CON_ANSI_CLR);

            printf("  %-19s", "P2P compatibility");

switch (nat_result.p2p_compatibility)
{
    case RR_P2P_COMPATIBILITY_GOOD:
        printf("%sGOOD%s\n",
               RRC_CON_ANSI_FG_BRIGHT_GREEN,
               RRC_CON_ANSI_CLR);
        break;

    case RR_P2P_COMPATIBILITY_FAIR:
        printf("%sFAIR%s\n",
               RRC_CON_ANSI_FG_BRIGHT_YELLOW,
               RRC_CON_ANSI_CLR);
        break;

    case RR_P2P_COMPATIBILITY_BAD:
        printf("%sBAD%s\n",
               RRC_CON_ANSI_FG_BRIGHT_RED,
               RRC_CON_ANSI_CLR);
        break;

    default:
        printf("%sUNKNOWN%s\n",
               RRC_CON_ANSI_FG_BRIGHT_RED,
               RRC_CON_ANSI_CLR);
        break;
            }

                rrc_wait_for_result_dismiss();

                break;
            }

            rrc_shutdown_check();
            usleep(RRC_WPAD_LOOP_TIMEOUT);
        }
    }
}

enum rrc_settings_result rrc_settings_display(void *xfb, struct rrc_settingsfile *stored_settings, struct rrc_result *result)
{
    *result = rrc_result_success;

    // Read the XML to extract all possible options for the entries.
    FILE *xml_file = fopen(RRC_RIIVO_XML_PATH, "r");
    if (!xml_file)
    {
        *result = rrc_result_create_error_errno(errno, "Failed to open " RRC_RIIVO_XML_PATH);
        return RRC_SETTINGS_ERROR;
    }
    mxml_node_t *xml_top = mxmlLoadFile(NULL, xml_file, NULL);

    mxml_node_t *xml_options = mxmlFindElement(xml_top, xml_top, "options", NULL, NULL, MXML_DESCEND);
    RRC_ASSERT(xml_options != NULL, "no <options> tag in xml");

    const char **my_stuff_options, **savegame_options;
    int my_stuff_options_count, savegame_options_count;
    const char *autoupdate_options[] = {"Disabled", "Enabled"};
    int autoupdate_option_count = sizeof(autoupdate_options) / sizeof(char *);

    struct rrc_result r;
    r = xml_find_option_choices(xml_options, xml_top, "My Stuff", &my_stuff_options, &my_stuff_options_count, &stored_settings->my_stuff);
    if (rrc_result_is_error(r))
    {
        *result = r;
        return RRC_SETTINGS_ERROR;
    }

    r = xml_find_option_choices(xml_options, xml_top, "Seperate Savegame", &savegame_options, &savegame_options_count, &stored_settings->separate_savegame);
    if (rrc_result_is_error(r))
    {
        *result = r;
        return RRC_SETTINGS_ERROR;
    }

    // Begin initializing the settings UI.
    rrc_con_clear(true);

    struct settings_entry entries[] = {
        {.type = ENTRY_TYPE_BUTTON, .label = launch_label},
        {.type = ENTRY_TYPE_BUTTON, .label = perform_updates_label, .margin_top = 1},
        {.type = ENTRY_TYPE_BUTTON, .label = manage_channel_installation_label, .margin_top = 1},
        {.type = ENTRY_TYPE_BUTTON, .label = extras_label, .margin_top = 1},
        {.type = ENTRY_TYPE_SELECT,
         .label = my_stuff_label,
         .options = my_stuff_options,
         .selected_option = &stored_settings->my_stuff,
         .initial_selected_option = stored_settings->my_stuff,
         .option_count = my_stuff_options_count,
         .margin_top = 1},
        {.type = ENTRY_TYPE_SELECT,
         .label = savegame_label,
         .options = savegame_options,
         .selected_option = &stored_settings->separate_savegame,
         .initial_selected_option = stored_settings->separate_savegame,
         .option_count = savegame_options_count},
        {.type = ENTRY_TYPE_SELECT,
         .label = autoupdate_label,
         .options = autoupdate_options,
         .selected_option = &stored_settings->auto_update,
         .initial_selected_option = stored_settings->auto_update,
         .option_count = autoupdate_option_count},

        {.type = ENTRY_TYPE_BUTTON, .label = save_label, .margin_top = 1},

        {.type = ENTRY_TYPE_BUTTON, .label = exit_label, .margin_top = 1},
    };
    const int entry_count = sizeof(entries) / sizeof(struct settings_entry);
    int selected_idx = 0;

    /* Used to show the end state of an operation e.g. updating or saving changes */
    char status_message[64] = "";
    int status_message_row = 0;
    int status_message_col = 0;

    // Used for padding the label string with spaces so that all options are aligned with each other.
    u32 max_label_len = 0;
    for (int i = 0; i < entry_count; i++)
    {
        // Do some minimal validation on select settings while we have to go through all entries to get the max label length anyway.
        if (entries[i].type == ENTRY_TYPE_SELECT && entries[i].options == NULL)
        {
            RRC_FATAL("'%s' is a select option but has a NULL pointer for its options array", entries[i].label)
        }

        if (entries[i].type == ENTRY_TYPE_SELECT && entries[i].option_count == 0)
        {
            RRC_FATAL("'%s' is a select option but has 0 options to select", entries[i].label);
        }

        if (entries[i].type == ENTRY_TYPE_SELECT && entries[i].selected_option == NULL)
        {
            RRC_FATAL("'%s' is a select option but has a NULL selected_option pointer (likely not initialized)", entries[i].label);
        }

        int len = strlen(entries[i].label);
        if (len > max_label_len)
        {
            max_label_len = len;
        }
    }

    while (1)
    {
        rrc_shutdown_check();
        int row = RRC_SETTINGS_ROW_START;
        bool has_unsaved_changes = false;

        for (int i = 0; i < entry_count; i++)
        {
            const struct settings_entry *entry = &entries[i];

            // add any extra "newlines" (which means just seek)
            row += entry->margin_top;
            rrc_con_clear_line(row);
            rrc_con_cursor_seek_to(row, 0);

            bool is_selected = selected_idx == i;

            if (is_selected)
            {
                printf(RRC_CON_ANSI_FG_BRIGHT_WHITE);
                printf(cursor_icon);
            }
            else
            {
                for (int i = 0; i < strlen(cursor_icon); i++)
                {
                    putc(' ', stdout);
                }
            }

            if (entry->label == extras_label && rr_server_status_has_alert())
            {
                printf("%sExtras (1)%s  ",
                       rr_server_status_alert_color(),
                       RRC_CON_ANSI_CLR);
            }
            else
            {
                printf("%s  ", entry->label);
            }

            if (entry->type == ENTRY_TYPE_SELECT)
            {
                int label_len = strlen(entry->label);

                if (label_len < max_label_len)
                {
                    for (int i = 0; i < max_label_len - label_len; i++)
                    {
                        putc(' ', stdout);
                    }
                }

                if (is_selected)
                {
                    printf("> ");
                }

                printf("%s", entry->options[*entry->selected_option]);

                if (is_selected)
                {
                    printf(" <");
                }

                if (*entry->selected_option != entry->initial_selected_option)
                {
                    has_unsaved_changes = true;
                    printf("%s *", RRC_CON_ANSI_FG_WHITE);
                }
            }

            printf(RRC_CON_ANSI_CLR);
            row++;
        }

        row += 2;

        if (has_unsaved_changes && strcmp(status_message, changes_saved_status) == 0)
        {
            // Reset the "changes saved" status message if we have unsaved changes.
            status_message[0] = 0;
        }

        rrc_con_cursor_seek_to(status_message_row, status_message_col);
        printf("%s", status_message);

        rrc_con_cursor_seek_to(rrc_con_get_rows() - 2, strlen(cursor_icon));
        printf("Use the D-Pad to navigate.");

        // use an inner loop just for scanning for button presses, rather than re-printing everything all the time
        // because the current scene will remain "static" until a button is pressed
        while (1)
        {
            struct pad_state pad = rrc_pad_buttons();

            if (rrc_pad_home_pressed(pad))
            {
                goto exit;
            }
            else if (rrc_pad_down_pressed(pad))
            {
                if (selected_idx < entry_count - 1)
                {
                    selected_idx++;
                }
                else
                {
                    selected_idx = 0;
                }
                break;
            }
            else if (rrc_pad_up_pressed(pad))
            {
                if (selected_idx > 0)
                {
                    selected_idx--;
                }
                else
                {
                    selected_idx = entry_count - 1;
                }
                break;
            }

            struct settings_entry *entry = &entries[selected_idx];

            if (rrc_pad_left_pressed(pad) && entry->type == ENTRY_TYPE_SELECT)
            {
                if (*entry->selected_option > 0)
                {
                    (*entry->selected_option)--;
                }
                else
                {
                    // user pressed left while already at the last option, wrap back to the end
                    *entry->selected_option = entry->option_count - 1;
                }
                break;
            }

            if (rrc_pad_right_pressed(pad) && entry->type == ENTRY_TYPE_SELECT)
            {
                (*entry->selected_option)++;

                if (*entry->selected_option >= entry->option_count)
                {
                    // user pressed right while already at the last option, wrap back to the first one
                    *entry->selected_option = 0;
                }
                break;
            }

            if (rrc_pad_a_pressed(pad))
            {
                if (entry->label == launch_label)
                {
                    if (has_unsaved_changes && prompt_save_unsaved_changes(xfb, entries, entry_count))
                    {
                        struct rrc_result res = rrc_settingsfile_store(stored_settings);
                        rrc_result_error_check_error_normal(res, xfb);
                    }

                    goto launch;
                }
                else if (entry->label == save_label)
                {
                    // We'll set a status message in either ok/err case, so set up the position here.
                    status_message_row = 13;
                    status_message_col = strlen(cursor_icon) + strlen(save_label) + 3;

                    struct rrc_result res = rrc_settingsfile_store(stored_settings);
                    if (rrc_result_is_error(res))
                    {
                        strncpy(status_message, changes_not_saved_status, sizeof(status_message));
                        rrc_result_error_check_error_normal(res, xfb);
                        break;
                    }

                    for (int i = 0; i < entry_count; i++)
                    {
                        if (entries[i].type == ENTRY_TYPE_SELECT)
                        {
                            entries[i].initial_selected_option = *entries[i].selected_option;
                        }
                    }

                    strncpy(status_message, changes_saved_status, sizeof(status_message));
                    break;
                }
                else if (entry->label == perform_updates_label)
                {
                    int update_count;
                    bool updated;
                    struct rrc_result update_res = rrc_update_do_updates(xfb, &update_count, &updated);

                    if (rrc_result_is_error(update_res))
                    {
                        rrc_result_error_check_error_normal(update_res, xfb);
                    }
                    else
                    {
                        if (update_count == 0)
                        {
                            strncpy(status_message, RRC_CON_ANSI_FG_BRIGHT_YELLOW "No updates available." RRC_CON_ANSI_CLR, sizeof(status_message));
                        }
                        else if (updated)
                        {
                            snprintf(status_message, sizeof(status_message), "%d updates were installed.", update_count);
                            char *lines[] = { status_message, "", "The channel will now exit to apply the updates."};
                            rrc_prompt_1_option(xfb, lines, 3, "OK");
                            goto exit;
                        }

                        status_message_row = 3;
                        status_message_col = strlen(cursor_icon) + strlen(perform_updates_label) + 3;
                    }

                    rrc_con_clear(true);

                    break;
                }
                else if (entry->label == manage_channel_installation_label)
                {
                    char *lines[] = {
                        "Hey!",
                        "",
                        "We didn't make this yet.",
                        "https://github.com/Retro-Rewind-Team/RR-Launcher/issues/29"};

                    rrc_prompt_1_option(xfb, lines, 4, "Sorry");
                    strncpy(status_message, RRC_CON_ANSI_FG_BRIGHT_MAGENTA "Oops" RRC_CON_ANSI_CLR, sizeof(status_message));
                    status_message_row = 5;
                    status_message_col = strlen(cursor_icon) + strlen(manage_channel_installation_label) + 3;

                    break;
                }
                else if (entry->label == extras_label)
                {
                    rrc_extras_display(xfb);
                    rrc_con_clear(true);
                    break;
                }
                else if (entry->label == exit_label)
                {
                    if (has_unsaved_changes && prompt_save_unsaved_changes(xfb, entries, entry_count))
                    {
                        struct rrc_result res = rrc_settingsfile_store(stored_settings);
                        rrc_result_error_check_error_normal(res, xfb);
                    }

                    goto exit;
                }
            }

            usleep(RRC_WPAD_LOOP_TIMEOUT);
        }
        usleep(RRC_WPAD_LOOP_TIMEOUT);
    }

    goto launch;

launch:
    CLEANUP
    return RRC_SETTINGS_LAUNCH;

exit:
    CLEANUP
    return RRC_SETTINGS_EXIT;
}
