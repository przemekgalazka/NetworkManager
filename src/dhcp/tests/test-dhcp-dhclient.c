/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2010 Red Hat, Inc.
 *
 */

#include "nm-default.h"

#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "nm-utils/nm-dedup-multi.h"

#include "NetworkManagerUtils.h"
#include "dhcp/nm-dhcp-dhclient-utils.h"
#include "dhcp/nm-dhcp-utils.h"
#include "nm-utils.h"
#include "nm-ip4-config.h"
#include "platform/nm-platform.h"

#include "nm-test-utils-core.h"

#define DEBUG 1

static const int IFINDEX = 5;

static void
test_config (const char *orig,
             const char *expected,
             int addr_family,
             const char *hostname,
             guint32 timeout,
             gboolean use_fqdn,
             const char *dhcp_client_id,
             GBytes *expected_new_client_id,
             const char *iface,
             const char *anycast_addr)
{
	gs_free char *new = NULL;
	gs_unref_bytes GBytes *client_id = NULL;
	gs_unref_bytes GBytes *new_client_id = NULL;

	if (dhcp_client_id) {
		client_id = nm_dhcp_utils_client_id_string_to_bytes (dhcp_client_id);
		g_assert (client_id);
	}

	new = nm_dhcp_dhclient_create_config (iface,
	                                      addr_family,
	                                      client_id,
	                                      anycast_addr,
	                                      hostname,
	                                      timeout,
	                                      use_fqdn,
	                                      "/path/to/dhclient.conf",
	                                      orig,
	                                      &new_client_id);
	g_assert (new != NULL);

	if (!nm_streq (new, expected)) {
		g_message ("\n* OLD ---------------------------------\n"
		           "%s"
		           "\n- NEW -----------------------------------\n"
		           "%s"
		           "\n+ EXPECTED ++++++++++++++++++++++++++++++\n"
		           "%s"
		           "\n^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n",
		           orig, new, expected);
	}
	g_assert_cmpstr (new, ==, expected);

	if (expected_new_client_id) {
		g_assert (new_client_id);
		g_assert (g_bytes_equal (new_client_id, expected_new_client_id));
	 } else
		g_assert (new_client_id == NULL);
}

/*****************************************************************************/

static const char *orig_missing_expected = \
	"# Created by NetworkManager\n"
	"\n\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_orig_missing (void)
{
	test_config (NULL, orig_missing_expected, AF_INET, NULL, 0, FALSE, NULL, NULL, "eth0", NULL);
}

/*****************************************************************************/

static const char *override_client_id_orig = \
	"send dhcp-client-identifier 00:30:04:20:7A:08;\n";

static const char *override_client_id_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n"
	"send dhcp-client-identifier 11:22:33:44:55:66; # added by NetworkManager\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_override_client_id (void)
{
	test_config (override_client_id_orig, override_client_id_expected,
	             AF_INET, NULL, 0, FALSE,
	             "11:22:33:44:55:66",
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *quote_client_id_expected = \
	"# Created by NetworkManager\n"
	"\n"
	"send dhcp-client-identifier \"1234\"; # added by NetworkManager\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_quote_client_id (void)
{
	test_config (NULL, quote_client_id_expected,
	             AF_INET, NULL, 0, FALSE,
	             "1234",
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *ascii_client_id_expected = \
	"# Created by NetworkManager\n"
	"\n"
	"send dhcp-client-identifier \"qb:cd:ef:12:34:56\"; # added by NetworkManager\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_ascii_client_id (void)
{
	test_config (NULL, ascii_client_id_expected,
	             AF_INET, NULL, 0, FALSE,
	             "qb:cd:ef:12:34:56",
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *hex_single_client_id_expected = \
	"# Created by NetworkManager\n"
	"\n"
	"send dhcp-client-identifier ab:cd:0e:12:34:56; # added by NetworkManager\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_hex_single_client_id (void)
{
	test_config (NULL, hex_single_client_id_expected,
	             AF_INET, NULL, 0, FALSE,
	             "ab:cd:e:12:34:56",
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *existing_hex_client_id_orig = \
	"send dhcp-client-identifier 00:30:04:20:7A:08;\n";

static const char *existing_hex_client_id_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n"
	"send dhcp-client-identifier 00:30:04:20:7A:08;\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_existing_hex_client_id (void)
{
	gs_unref_bytes GBytes *new_client_id = NULL;
	const guint8 bytes[] = { 0x00, 0x30, 0x04,0x20, 0x7A, 0x08 };

	new_client_id = g_bytes_new (bytes, sizeof (bytes));
	test_config (existing_hex_client_id_orig, existing_hex_client_id_expected,
	             AF_INET, NULL, 0, FALSE,
	             NULL,
	             new_client_id,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

#define EACID "qb:cd:ef:12:34:56"

static const char *existing_ascii_client_id_orig = \
	"send dhcp-client-identifier \"" EACID "\";\n";

static const char *existing_ascii_client_id_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n"
	"send dhcp-client-identifier \"" EACID "\";\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_existing_ascii_client_id (void)
{
	gs_unref_bytes GBytes *new_client_id = NULL;
	char buf[NM_STRLEN (EACID) + 1] = { 0 };

	memcpy (buf + 1, EACID, NM_STRLEN (EACID));
	new_client_id = g_bytes_new (buf, sizeof (buf));
	test_config (existing_ascii_client_id_orig, existing_ascii_client_id_expected,
	             AF_INET, NULL, 0, FALSE,
	             NULL,
	             new_client_id,
	             "eth0",
	             NULL);
}
/*****************************************************************************/

static const char *fqdn_expected = \
	"# Created by NetworkManager\n"
	"\n"
	"send fqdn.fqdn \"foo.bar.com\"; # added by NetworkManager\n"
	"send fqdn.encoded on;\n"
	"send fqdn.server-update on;\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n\n";

static void
test_fqdn (void)
{
	test_config (NULL, fqdn_expected,
	             AF_INET, "foo.bar.com", 0,
	             TRUE, NULL,
	             NULL,
	             "eth0",
	             NULL);
}

static const char *fqdn_options_override_orig = \
	"\n"
	"send fqdn.fqdn \"foobar.com\"\n"    /* NM must ignore this ... */
	"send fqdn.encoded off;\n"           /* ... and honor these */
	"send fqdn.server-update off;\n";

static const char *fqdn_options_override_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n"
	"send fqdn.fqdn \"example2.com\"; # added by NetworkManager\n"
	"send fqdn.encoded on;\n"
	"send fqdn.server-update on;\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n"
	"# FQDN options from /path/to/dhclient.conf\n"
	"send fqdn.encoded off;\n"
	"send fqdn.server-update off;\n\n";

static void
test_fqdn_options_override (void)
{
	test_config (fqdn_options_override_orig,
	             fqdn_options_override_expected,
	             AF_INET, "example2.com", 0,
	             TRUE, NULL,
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *override_hostname_orig = \
	"send host-name \"foobar\";\n";

static const char *override_hostname_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n"
	"send host-name \"blahblah\"; # added by NetworkManager\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_override_hostname (void)
{
	test_config (override_hostname_orig, override_hostname_expected,
	             AF_INET, "blahblah", 0, FALSE,
	             NULL,
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *override_hostname6_orig = \
	"send fqdn.fqdn \"foobar\";\n";

static const char *override_hostname6_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n"
	"send fqdn.fqdn \"blahblah.local\"; # added by NetworkManager\n"
	"send fqdn.server-update on;\n"
	"\n"
	"also request dhcp6.name-servers;\n"
	"also request dhcp6.domain-search;\n"
	"also request dhcp6.client-id;\n"
	"\n";

static void
test_override_hostname6 (void)
{
	test_config (override_hostname6_orig, override_hostname6_expected,
	             AF_INET6, "blahblah.local", 0, TRUE,
	             NULL,
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *nonfqdn_hostname6_expected = \
	"# Created by NetworkManager\n"
	"\n"
	"send fqdn.fqdn \"blahblah\"; # added by NetworkManager\n"
	"send fqdn.server-update on;\n"
	"\n"
	"also request dhcp6.name-servers;\n"
	"also request dhcp6.domain-search;\n"
	"also request dhcp6.client-id;\n"
	"\n";

static void
test_nonfqdn_hostname6 (void)
{
	/* Non-FQDN hostname can now be used with dhclient */
	test_config (NULL, nonfqdn_hostname6_expected,
	             AF_INET6, "blahblah", 0, TRUE,
	             NULL,
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *existing_alsoreq_orig = \
	"also request something;\n"
	"also request another-thing;\n"
	;

static const char *existing_alsoreq_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request something;\n"
	"also request another-thing;\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_existing_alsoreq (void)
{
	test_config (existing_alsoreq_orig, existing_alsoreq_expected,
	             AF_INET, NULL, 0, FALSE,
	             NULL,
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *existing_req_orig = \
	"request something;\n"
	"also request some-other-thing;\n"
	"request another-thing;\n"
	"also request yet-another-thing;\n"
	;

static const char *existing_req_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"request; # override dhclient defaults\n"
	"also request another-thing;\n"
	"also request yet-another-thing;\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_existing_req (void)
{
	test_config (existing_req_orig, existing_req_expected,
	             AF_INET, NULL, 0, FALSE,
	             NULL,
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *existing_multiline_alsoreq_orig = \
	"also request something another-thing yet-another-thing\n"
	"    foobar baz blah;\n"
	;

static const char *existing_multiline_alsoreq_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request something;\n"
	"also request another-thing;\n"
	"also request yet-another-thing;\n"
	"also request foobar;\n"
	"also request baz;\n"
	"also request blah;\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_existing_multiline_alsoreq (void)
{
	test_config (existing_multiline_alsoreq_orig, existing_multiline_alsoreq_expected,
	             AF_INET, NULL, 0, FALSE,
	             NULL,
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static void
test_one_duid (const char *escaped, const guint8 *unescaped, guint len)
{
	GByteArray *t;
	char *w;

	t = nm_dhcp_dhclient_unescape_duid (escaped);
	g_assert (t);
	g_assert_cmpint (t->len, ==, len);
	g_assert_cmpint (memcmp (t->data, unescaped, len), ==, 0);
	g_byte_array_free (t, TRUE);

	t = g_byte_array_sized_new (len);
	g_byte_array_append (t, unescaped, len);
	w = nm_dhcp_dhclient_escape_duid (t);
	g_assert (w);
	g_assert_cmpint (strlen (escaped), ==, strlen (w));
	g_assert_cmpstr (escaped, ==, w);

	g_byte_array_free (t, TRUE);
	g_free (w);
}

static void
test_duids (void)
{
	const guint8 test1_u[] = { 0x00, 0x01, 0x00, 0x01, 0x13, 0x6f, 0x13, 0x6e,
	                           0x00, 0x22, 0xfa, 0x8c, 0xd6, 0xc2 };
	const char *test1_s = "\\000\\001\\000\\001\\023o\\023n\\000\\\"\\372\\214\\326\\302";

	const guint8 test2_u[] = { 0x00, 0x01, 0x00, 0x01, 0x17, 0x57, 0xee, 0x39,
	                           0x00, 0x23, 0x15, 0x08, 0x7E, 0xac };
	const char *test2_s = "\\000\\001\\000\\001\\027W\\3569\\000#\\025\\010~\\254";

	const guint8 test3_u[] = { 0x00, 0x01, 0x00, 0x01, 0x17, 0x58, 0xe8, 0x58,
	                           0x00, 0x23, 0x15, 0x08, 0x7e, 0xac };
	const char *test3_s = "\\000\\001\\000\\001\\027X\\350X\\000#\\025\\010~\\254";

	const guint8 test4_u[] = { 0x00, 0x01, 0x00, 0x01, 0x15, 0xd5, 0x31, 0x97,
	                           0x00, 0x16, 0xeb, 0x04, 0x45, 0x18 };
	const char *test4_s = "\\000\\001\\000\\001\\025\\3251\\227\\000\\026\\353\\004E\\030";

	const char *bad_s = "\\000\\001\\000\\001\\425\\3251\\227\\000\\026\\353\\004E\\030";

	test_one_duid (test1_s, test1_u, sizeof (test1_u));
	test_one_duid (test2_s, test2_u, sizeof (test2_u));
	test_one_duid (test3_s, test3_u, sizeof (test3_u));
	test_one_duid (test4_s, test4_u, sizeof (test4_u));

	/* Invalid octal digit */
	g_assert (nm_dhcp_dhclient_unescape_duid (bad_s) == NULL);
}

static void
test_read_duid_from_leasefile (void)
{
	const guint8 expected[] = { 0x00, 0x01, 0x00, 0x01, 0x18, 0x79, 0xa6,
	                            0x13, 0x60, 0x67, 0x20, 0xec, 0x4c, 0x70 };
	GByteArray *duid;
	GError *error = NULL;

	duid = nm_dhcp_dhclient_read_duid (TESTDIR "/test-dhclient-duid.leases", &error);
	g_assert_no_error (error);
	g_assert (duid);
	g_assert_cmpint (duid->len, ==, sizeof (expected));
	g_assert_cmpint (memcmp (duid->data, expected, duid->len), ==, 0);

	g_byte_array_free (duid, TRUE);
}

static void
test_read_commented_duid_from_leasefile (void)
{
	GByteArray *duid;
	GError *error = NULL;

	duid = nm_dhcp_dhclient_read_duid (TESTDIR "/test-dhclient-commented-duid.leases", &error);
	g_assert_no_error (error);
	g_assert (duid == NULL);
}

static void
test_write_duid (void)
{
	const char *duid = "\\000\\001\\000\\001\\027X\\350X\\000#\\025\\010~\\254";
	const char *expected_contents = "default-duid \"\\000\\001\\000\\001\\027X\\350X\\000#\\025\\010~\\254\";\n";
	GError *error = NULL;
	char *contents = NULL;
	gboolean success;
	const char *path = "test-dhclient-write-duid.leases";

	success = nm_dhcp_dhclient_save_duid (path, duid, &error);
	g_assert_no_error (error);
	g_assert (success);

	success = g_file_get_contents (path, &contents, NULL, &error);
	g_assert_no_error (error);
	g_assert (success);

	unlink (path);
	g_assert_cmpstr (expected_contents, ==, contents);

	g_free (contents);
}

static void
test_write_existing_duid (void)
{
	const char *duid = "\\000\\001\\000\\001\\023o\\023n\\000\\\"\\372\\214\\326\\302";
	const char *expected_contents = "default-duid \"\\000\\001\\000\\001\\027X\\350X\\000#\\025\\010~\\254\";\n";
	GError *error = NULL;
	char *contents = NULL;
	gboolean success;
	const char *path = "test-dhclient-write-existing-duid.leases";

	success = g_file_set_contents (path, expected_contents, -1, &error);
	g_assert_no_error (error);
	g_assert (success);

	/* Save other DUID; should be a no-op */
	success = nm_dhcp_dhclient_save_duid (path, duid, &error);
	g_assert_no_error (error);
	g_assert (success);

	/* reread original contents */
	success = g_file_get_contents (path, &contents, NULL, &error);
	g_assert_no_error (error);
	g_assert (success);

	unlink (path);
	g_assert_cmpstr (expected_contents, ==, contents);

	g_free (contents);
}

static void
test_write_existing_commented_duid (void)
{
	#define DUID "\\000\\001\\000\\001\\023o\\023n\\000\\\"\\372\\214\\326\\302"
	#define ORIG_CONTENTS "#default-duid \"\\000\\001\\000\\001\\027X\\350X\\000#\\025\\010~\\254\";\n"
	const char *expected_contents = \
		"default-duid \"" DUID "\";\n"
		ORIG_CONTENTS;
	GError *error = NULL;
	char *contents = NULL;
	gboolean success;
	const char *path = "test-dhclient-write-existing-commented-duid.leases";

	success = g_file_set_contents (path, ORIG_CONTENTS, -1, &error);
	g_assert_no_error (error);
	g_assert (success);

	/* Save other DUID; should be a no-op */
	success = nm_dhcp_dhclient_save_duid (path, DUID, &error);
	g_assert_no_error (error);
	g_assert (success);

	/* reread original contents */
	success = g_file_get_contents (path, &contents, NULL, &error);
	g_assert_no_error (error);
	g_assert (success);

	unlink (path);
	g_assert_cmpstr (expected_contents, ==, contents);

	g_free (contents);
}

/*****************************************************************************/

static const char *interface1_orig = \
	"interface \"eth0\" {\n"
	"	also request my-option;\n"
	"	initial-delay 5;\n"
	"}\n"
	"interface \"eth1\" {\n"
	"	also request another-option;\n"
	"	initial-delay 0;\n"
	"}\n"
	"\n"
	"also request yet-another-option;\n";

static const char *interface1_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n"
	"initial-delay 5;\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"also request my-option;\n"
	"also request yet-another-option;\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_interface1 (void)
{
	test_config (interface1_orig, interface1_expected,
	             AF_INET, NULL, 0, FALSE,
	             NULL,
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static const char *interface2_orig = \
	"interface eth0 {\n"
	"	also request my-option;\n"
	"	initial-delay 5;\n"
	" }\n"
	"interface eth1 {\n"
	"	initial-delay 0;\n"
	"	request another-option;\n"
	" } \n"
	"\n"
	"also request yet-another-option;\n";

static const char *interface2_expected = \
	"# Created by NetworkManager\n"
	"# Merged from /path/to/dhclient.conf\n"
	"\n"
	"initial-delay 0;\n"
	"\n"
	"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
	"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
	"option wpad code 252 = string;\n"
	"\n"
	"request; # override dhclient defaults\n"
	"also request another-option;\n"
	"also request yet-another-option;\n"
	"also request rfc3442-classless-static-routes;\n"
	"also request ms-classless-static-routes;\n"
	"also request static-routes;\n"
	"also request wpad;\n"
	"also request ntp-servers;\n"
	"\n";

static void
test_interface2 (void)
{
	test_config (interface2_orig, interface2_expected,
	             AF_INET, NULL, 0, FALSE,
	             NULL,
	             NULL,
	             "eth1",
	             NULL);
}

static void
test_config_req_intf (void)
{
	static const char *const orig = \
		"request subnet-mask, broadcast-address, routers,\n"
		"	rfc3442-classless-static-routes,\n"
		"	interface-mtu, host-name, domain-name, domain-search,\n"
		"	domain-name-servers, nis-domain, nis-servers,\n"
		"	nds-context, nds-servers, nds-tree-name,\n"
		"	netbios-name-servers, netbios-dd-server,\n"
		"	netbios-node-type, netbios-scope, ntp-servers;\n"
		"";
	static const char *const expected = \
		"# Created by NetworkManager\n"
		"# Merged from /path/to/dhclient.conf\n"
		"\n"
		"\n"
		"option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;\n"
		"option ms-classless-static-routes code 249 = array of unsigned integer 8;\n"
		"option wpad code 252 = string;\n"
		"\n"
		"request; # override dhclient defaults\n"
		"also request subnet-mask;\n"
		"also request broadcast-address;\n"
		"also request routers;\n"
		"also request rfc3442-classless-static-routes;\n"
		"also request interface-mtu;\n"
		"also request host-name;\n"
		"also request domain-name;\n"
		"also request domain-search;\n"
		"also request domain-name-servers;\n"
		"also request nis-domain;\n"
		"also request nis-servers;\n"
		"also request nds-context;\n"
		"also request nds-servers;\n"
		"also request nds-tree-name;\n"
		"also request netbios-name-servers;\n"
		"also request netbios-dd-server;\n"
		"also request netbios-node-type;\n"
		"also request netbios-scope;\n"
		"also request ntp-servers;\n"
		"also request ms-classless-static-routes;\n"
		"also request static-routes;\n"
		"also request wpad;\n"
		"\n";

	test_config (orig, expected,
	             AF_INET, NULL, 0, FALSE,
	             NULL,
	             NULL,
	             "eth0",
	             NULL);
}

/*****************************************************************************/

static void
test_read_lease_ip4_config_basic (void)
{
	nm_auto_unref_dedup_multi_index NMDedupMultiIndex *multi_idx = nm_dedup_multi_index_new ();
	GError *error = NULL;
	char *contents = NULL;
	gboolean success;
	const char *path = TESTDIR "/leases/basic.leases";
	GSList *leases;
	GDateTime *now;
	NMIP4Config *config;
	const NMPlatformIP4Address *addr;
	guint32 expected_addr;

	success = g_file_get_contents (path, &contents, NULL, &error);
	g_assert_no_error (error);
	g_assert (success);

	/* Date from before the least expiration */
	now = g_date_time_new_utc (2013, 11, 1, 19, 55, 32);
	leases = nm_dhcp_dhclient_read_lease_ip_configs (multi_idx, AF_INET, "wlan0", IFINDEX, contents, now);
	g_assert_cmpint (g_slist_length (leases), ==, 2);

	/* IP4Config #1 */
	config = g_slist_nth_data (leases, 0);
	g_assert (NM_IS_IP4_CONFIG (config));

	/* Address */
	g_assert_cmpint (nm_ip4_config_get_num_addresses (config), ==, 1);
	expected_addr = nmtst_inet4_from_string ("192.168.1.180");
	addr = _nmtst_ip4_config_get_address (config, 0);
	g_assert_cmpint (addr->address, ==, expected_addr);
	g_assert_cmpint (addr->peer_address, ==, expected_addr);
	g_assert_cmpint (addr->plen, ==, 24);

	/* Gateway */
	expected_addr = nmtst_inet4_from_string ("192.168.1.1");
	g_assert_cmpint (nm_ip4_config_get_gateway (config), ==, expected_addr);

	/* DNS */
	g_assert_cmpint (nm_ip4_config_get_num_nameservers (config), ==, 1);
	expected_addr = nmtst_inet4_from_string ("192.168.1.1");
	g_assert_cmpint (nm_ip4_config_get_nameserver (config, 0), ==, expected_addr);

	g_assert_cmpint (nm_ip4_config_get_num_domains (config), ==, 0);

	/* IP4Config #2 */
	config = g_slist_nth_data (leases, 1);
	g_assert (NM_IS_IP4_CONFIG (config));

	/* Address */
	g_assert_cmpint (nm_ip4_config_get_num_addresses (config), ==, 1);
	expected_addr = nmtst_inet4_from_string ("10.77.52.141");
	addr = _nmtst_ip4_config_get_address (config, 0);
	g_assert_cmpint (addr->address, ==, expected_addr);
	g_assert_cmpint (addr->peer_address, ==, expected_addr);
	g_assert_cmpint (addr->plen, ==, 8);

	/* Gateway */
	expected_addr = nmtst_inet4_from_string ("10.77.52.254");
	g_assert_cmpint (nm_ip4_config_get_gateway (config), ==, expected_addr);

	/* DNS */
	g_assert_cmpint (nm_ip4_config_get_num_nameservers (config), ==, 2);
	expected_addr = nmtst_inet4_from_string ("8.8.8.8");
	g_assert_cmpint (nm_ip4_config_get_nameserver (config, 0), ==, expected_addr);
	expected_addr = nmtst_inet4_from_string ("8.8.4.4");
	g_assert_cmpint (nm_ip4_config_get_nameserver (config, 1), ==, expected_addr);

	/* Domains */
	g_assert_cmpint (nm_ip4_config_get_num_domains (config), ==, 1);
	g_assert_cmpstr (nm_ip4_config_get_domain (config, 0), ==, "morriesguest.local");

	g_slist_free_full (leases, g_object_unref);
	g_date_time_unref (now);
	g_free (contents);
}

static void
test_read_lease_ip4_config_expired (void)
{
	nm_auto_unref_dedup_multi_index NMDedupMultiIndex *multi_idx = nm_dedup_multi_index_new ();
	GError *error = NULL;
	char *contents = NULL;
	gboolean success;
	const char *path = TESTDIR "/leases/basic.leases";
	GSList *leases;
	GDateTime *now;

	success = g_file_get_contents (path, &contents, NULL, &error);
	g_assert_no_error (error);
	g_assert (success);

	/* Date from *after* the lease expiration */
	now = g_date_time_new_utc (2013, 12, 1, 19, 55, 32);
	leases = nm_dhcp_dhclient_read_lease_ip_configs (multi_idx, AF_INET, "wlan0", IFINDEX, contents, now);
	g_assert (leases == NULL);

	g_date_time_unref (now);
	g_free (contents);
}

static void
test_read_lease_ip4_config_expect_failure (gconstpointer user_data)
{
	nm_auto_unref_dedup_multi_index NMDedupMultiIndex *multi_idx = nm_dedup_multi_index_new ();
	GError *error = NULL;
	char *contents = NULL;
	gboolean success;
	GSList *leases;
	GDateTime *now;

	success = g_file_get_contents ((const char *) user_data, &contents, NULL, &error);
	g_assert_no_error (error);
	g_assert (success);

	/* Date from before the least expiration */
	now = g_date_time_new_utc (2013, 11, 1, 1, 1, 1);
	leases = nm_dhcp_dhclient_read_lease_ip_configs (multi_idx, AF_INET, "wlan0", IFINDEX, contents, now);
	g_assert (leases == NULL);

	g_date_time_unref (now);
	g_free (contents);
}

/*****************************************************************************/

NMTST_DEFINE ();

int
main (int argc, char **argv)
{
	nmtst_init_with_logging (&argc, &argv, NULL, "DEFAULT");

	g_test_add_func ("/dhcp/dhclient/orig_missing", test_orig_missing);
	g_test_add_func ("/dhcp/dhclient/override_client_id", test_override_client_id);
	g_test_add_func ("/dhcp/dhclient/quote_client_id", test_quote_client_id);
	g_test_add_func ("/dhcp/dhclient/ascii_client_id", test_ascii_client_id);
	g_test_add_func ("/dhcp/dhclient/hex_single_client_id", test_hex_single_client_id);
	g_test_add_func ("/dhcp/dhclient/existing-hex-client-id", test_existing_hex_client_id);
	g_test_add_func ("/dhcp/dhclient/existing-ascii-client-id", test_existing_ascii_client_id);
	g_test_add_func ("/dhcp/dhclient/fqdn", test_fqdn);
	g_test_add_func ("/dhcp/dhclient/fqdn_options_override", test_fqdn_options_override);
	g_test_add_func ("/dhcp/dhclient/override_hostname", test_override_hostname);
	g_test_add_func ("/dhcp/dhclient/override_hostname6", test_override_hostname6);
	g_test_add_func ("/dhcp/dhclient/nonfqdn_hostname6", test_nonfqdn_hostname6);
	g_test_add_func ("/dhcp/dhclient/existing_req", test_existing_req);
	g_test_add_func ("/dhcp/dhclient/existing_alsoreq", test_existing_alsoreq);
	g_test_add_func ("/dhcp/dhclient/existing_multiline_alsoreq", test_existing_multiline_alsoreq);
	g_test_add_func ("/dhcp/dhclient/duids", test_duids);
	g_test_add_func ("/dhcp/dhclient/interface/1", test_interface1);
	g_test_add_func ("/dhcp/dhclient/interface/2", test_interface2);
	g_test_add_func ("/dhcp/dhclient/config/req_intf", test_config_req_intf);

	g_test_add_func ("/dhcp/dhclient/read_duid_from_leasefile", test_read_duid_from_leasefile);
	g_test_add_func ("/dhcp/dhclient/read_commented_duid_from_leasefile", test_read_commented_duid_from_leasefile);

	g_test_add_func ("/dhcp/dhclient/write_duid", test_write_duid);
	g_test_add_func ("/dhcp/dhclient/write_existing_duid", test_write_existing_duid);
	g_test_add_func ("/dhcp/dhclient/write_existing_commented_duid", test_write_existing_commented_duid);

	g_test_add_func ("/dhcp/dhclient/leases/ip4-config/basic", test_read_lease_ip4_config_basic);
	g_test_add_func ("/dhcp/dhclient/leases/ip4-config/expired", test_read_lease_ip4_config_expired);
	g_test_add_data_func ("/dhcp/dhclient/leases/ip4-config/missing-address",
	                      TESTDIR "/leases/malformed1.leases",
	                      test_read_lease_ip4_config_expect_failure);
	g_test_add_data_func ("/dhcp/dhclient/leases/ip4-config/missing-gateway",
	                      TESTDIR "/leases/malformed2.leases",
	                      test_read_lease_ip4_config_expect_failure);
	g_test_add_data_func ("/dhcp/dhclient/leases/ip4-config/missing-expire",
	                      TESTDIR "/leases/malformed3.leases",
	                      test_read_lease_ip4_config_expect_failure);

	return g_test_run ();
}

