/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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
 * Copyright (C) 2017 Red Hat, Inc.
 */

#include "nm-default.h"

#include <string.h>
#include <jansson.h>
#include <gmodule.h>
#include <gio/gunixsocketaddress.h>

#include "nm-openvswitch-factory.h"
#include "nm-device-openvswitch.h"
#include "platform/nm-platform.h"
#include "nm-default.h"
#include "nm-core-internal.h"

/*****************************************************************************/

typedef struct {
	char *name;
	GPtrArray *interfaces;		/* interface uuids */
} OpenvswitchPort;

typedef struct {
	char *name;
	GPtrArray *ports;		/* port uuids */
} OpenvswitchBridge;

typedef struct {
	GSocketClient *client;
	GSocketConnection *conn;
	GCancellable *cancellable;
	char buf[4096];			/* Input buffer */
	size_t bufp;			/* Last decoded byte in the input buffer. */
	GString *input;			/* JSON stream waiting for decoding. */
	GString *output;		/* JSON stream to be sent. */
	guint64 seq;
	GArray *calls;			/* Method calls waiting for a response. */
	GHashTable *interfaces;		/* interface uuid => interface name */
	GHashTable *ports;		/* port uuid => OpenvswitchBridge */
	GHashTable *bridges;		/* bridge uuid => OpenvswitchBridge */
	const char *db_uuid;
} NMOpenvswitchFactoryPrivate;

struct _NMOpenvswitchFactory {
	NMDeviceFactory parent;
	NMOpenvswitchFactoryPrivate _priv;
};

struct _NMOpenvswitchFactoryClass {
	NMDeviceFactoryClass parent;
};

G_DEFINE_TYPE (NMOpenvswitchFactory, nm_openvswitch_factory, NM_TYPE_DEVICE_FACTORY)

#define NM_OPENVSWITCH_FACTORY_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMOpenvswitchFactory, NM_IS_OPENVSWITCH_FACTORY)

/*****************************************************************************/

#define _NMLOG_DOMAIN      LOGD_DEVICE
#define _NMLOG(level, ...) __NMLOG_DEFAULT (level, _NMLOG_DOMAIN, "openvswitch", __VA_ARGS__)

/*****************************************************************************/

static void ovsdb_try_connect (NMOpenvswitchFactory *self);
static void ovsdb_disconnect (NMOpenvswitchFactory *self);
static void ovsdb_read (NMOpenvswitchFactory *self);
static void ovsdb_write (NMOpenvswitchFactory *self);
static void ovsdb_next_command (NMOpenvswitchFactory *self);

/*****************************************************************************/

/* ovsdb command abstraction. */

typedef void (*OvsdbMethodCallback) (NMOpenvswitchFactory *self, json_t *response,
                                     GError *error, gpointer user_data);

typedef enum {
	OVSDB_MONITOR,
	OVSDB_ADD_BR,
	OVSDB_DEL_BR,
	OVSDB_ADD_PORT,
	OVSDB_DEL_PORT,
} OvsdbCommand;

typedef struct {
	guint64 id;
	OvsdbMethodCallback callback;
	gpointer user_data;
	OvsdbCommand command;
	char bridge_iface[IFNAMSIZ + 1];	/* Used by add and del commands. */
	char port_iface[IFNAMSIZ + 1];		/* Used by port commands. */
} OvsdbMethodCall;

/**
 * ovsdb_call_method:
 *
 * Queues the ovsdb command. Eventually fires the command right away if
 * there's no command pending completion.
 */
static void
ovsdb_call_method (NMOpenvswitchFactory *self, OvsdbCommand command,
                   const char *bridge_iface, const char *port_iface,
                   OvsdbMethodCallback callback, gpointer user_data)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	OvsdbMethodCall *call;

	/* Ensure we're not unsynchronized before we queue the method call. */
	ovsdb_try_connect (self);

	g_array_set_size (priv->calls, priv->calls->len + 1);
	call = &g_array_index (priv->calls, OvsdbMethodCall, priv->calls->len - 1);
	call->id = priv->seq++;
	call->command = command;
	if (bridge_iface) {
		/* The add and del commands use a bridge_iface parameter. */
		strcpy (call->bridge_iface, bridge_iface);
	}
	if (port_iface) {
		/* The port commands use a port_iface parameter. */
		strcpy (call->port_iface, port_iface);
	}
	call->callback = callback;
	call->user_data = user_data;

	if (priv->calls->len == 1) {
		/* There was no command waiting for completion -- we're free
		 * to ahead and proceed serializing and write this one without
		 * waiting for a command to complete. */
		ovsdb_next_command (self);
	}
}

/*****************************************************************************/

/* Create and process the JSON-RPC messages from ovsdb. */

/*
 * _fill_ports:
 *
 * Put set of all ports of @bridge_iface into @items and all but
 * @exclude_port_iface into @new_items.
 */
static void
_fill_ports (NMOpenvswitchFactory *self,
             const char *bridge_iface, const char *exclude_port_iface,
             json_t **items, json_t **new_items)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	GHashTableIter iter;
	char *bridge_uuid;
	char *port_uuid;
	OpenvswitchBridge *ovs_bridge;
	OpenvswitchPort *ovs_port;
	int i;

	*items = json_array ();
	*new_items = json_array ();

	g_hash_table_iter_init (&iter, priv->bridges);
	while (g_hash_table_iter_next (&iter, (gpointer) &bridge_uuid, (gpointer) &ovs_bridge)) {
		if (g_strcmp0 (ovs_bridge->name, bridge_iface) != 0)
			continue;
		for (i = 0; i < ovs_bridge->ports->len; i++) {
			port_uuid = g_ptr_array_index (ovs_bridge->ports, i);
			json_array_append_new (*items, json_pack ("[s,s]", "uuid", port_uuid));

			ovs_port = g_hash_table_lookup (priv->ports, port_uuid);
			if (!ovs_port)
				continue;
			if (g_strcmp0 (exclude_port_iface, ovs_port->name) == 0)
				continue;
			json_array_append_new (*new_items, json_pack ("[s,s]", "uuid", port_uuid));
		}
	}
}

/**
 * _fill_bridges:
 *
 * Put set of all bridges into @items and all but @exclude_bridge_iface into
 * @new_items.
 */
static void
_fill_bridges (NMOpenvswitchFactory *self, const char *exclude_bridge_iface,
               json_t **items, json_t **new_items)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	GHashTableIter iter;
	char *bridge_uuid;
	OpenvswitchBridge *ovs_bridge;

	*items = json_array ();
	*new_items = json_array ();

	g_hash_table_iter_init (&iter, priv->bridges);
	while (g_hash_table_iter_next (&iter, (gpointer) &bridge_uuid, (gpointer) &ovs_bridge)) {
		json_array_append_new (*items, json_pack ("[s,s]", "uuid", bridge_uuid));
		if (g_strcmp0 (exclude_bridge_iface, ovs_bridge->name) != 0)
			json_array_append_new (*new_items, json_pack ("[s,s]", "uuid", bridge_uuid));
	}
}

/**
 * _expect_ports:
 *
 * Return a command that will fail the transaction if the actual set of
 * ports in @bridge_iface doesn't match @ports. This is a way of detecting
 * race conditions with other ovsdb clients that might be adding or removing
 * bridge ports at the same time.
 */
static json_t *
_expect_ports (const char *bridge_iface, const json_t *ports)
{
	return json_pack ("{s:s, s:s, s:i, s:[s], s:s, s:[{s:[s, o]}], s:[[s, s, s]]}",
	                  "op", "wait", "table", "Bridge",
	                  "timeout", 0, "columns", "ports",
	                  "until", "==", "rows", "ports", "set", ports,
	                  "where", "name", "==", bridge_iface);
}

/**
 * _set_ports:
 *
 * Return a command that will update the list of ports of @bridge_iface
 * to @ports.
 */
static json_t *
_set_ports (const char *bridge_iface, const json_t *ports)
{
	return json_pack ("{s:s, s:s, s:{s:[s, o]}, s:[[s, s, s]]}",
	                  "op", "update", "table", "Bridge",
	                   "row", "ports", "set", ports,
	                  "where", "name", "==", bridge_iface);
}

/**
 * _expect_bridges:
 *
 * Return a command that will fail the transaction if the actual set of
 * bridges doesn't match @bridges. This is a way of detecting race conditions
 * with other ovsdb clients that might be adding or removing bridges
 * at the same time.
 */
static json_t *
_expect_bridges (json_t *bridges, const char *db_uuid)
{
	return json_pack ("{s:s, s:s, s:i, s:[s], s:s, s:[{s:[s, o]}], s:[[s, s, [s, s]]]}",
	                  "op", "wait", "table", "Open_vSwitch",
	                  "timeout", 0, "columns", "bridges",
	                  "until", "==", "rows", "bridges", "set", bridges,
	                  "where", "_uuid", "==", "uuid", db_uuid);
}

/**
 * _set_bridges:
 *
 * Return a command that will update the list of bridges in @db_uuid
 * database to @bridges.
 */
static json_t *
_set_bridges (const json_t *bridges, const char *db_uuid)
{
	return json_pack ("{s:s, s:s, s:{s:[s, o]}, s:[[s, s, [s, s]]]}",
	                  "op", "update", "table", "Open_vSwitch",
	                   "row", "bridges", "set", bridges,
	                  "where", "_uuid", "==", "uuid", db_uuid);
}

/**
 * _inc_next_cfg:
 *
 * Returns an mutate commands that bumps next_cfg upon successful completion
 * of the transaction it is in.
 */
static json_t *
_inc_next_cfg (const char *db_uuid)
{
	return json_pack ("{s:s, s:s, s:[[s, s, i]], s:[[s, s, [s, s]]]}",
                          "op", "mutate", "table", "Open_vSwitch",
	                  "mutations", "next_cfg", "+=", 1,
	                  "where", "_uuid", "==", "uuid", db_uuid);
}

/**
 * ovsdb_next_command:
 *
 * Translates a higher level operation (add/remove bridge/port) to a RFC 7047
 * command serialized into JSON ands sends it over to the database.

 * Only called when no command is waiting for a response, since the serialized
 * command might depend on result of a previous one (add and remove need to
 * include an up to date bridge list in their transactions to rule out races).
 */
static void
ovsdb_next_command (NMOpenvswitchFactory *self)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	OvsdbMethodCall *call = NULL;
	char *cmd;
	json_t *msg = NULL;
	json_t *items, *new_items;

	if (!priv->conn)
		return;
	if (!priv->calls->len)
		return;
	call = &g_array_index (priv->calls, OvsdbMethodCall, 0);

	switch (call->command) {
	case OVSDB_MONITOR:
		msg = json_pack ("{s:i, s:s, s:[s, n, {"
		                 "  s:[{s:[s, s]}],"
		                 "  s:[{s:[s, s]}],"
		                 "  s:[{s:[s]}],"
		                 "  s:[{s:[]}]"
		                 "}]}",
		                 "id", call->id,
		                 "method", "monitor", "params", "Open_vSwitch",
		                 "Bridge", "columns", "name", "ports",
		                 "Port", "columns", "name", "interfaces",
		                 "Interface", "columns", "name",
		                 "Open_vSwitch", "columns");
		break;
	case OVSDB_ADD_BR:
		_fill_bridges (self, call->bridge_iface, &items, &new_items);
		json_array_append_new (new_items, json_pack ("[s,s]", "named-uuid", "rowBridge"));

		msg = json_pack ("{s:i, s:s, s:[s,o,o,o"
		                 "  {s:s, s:s, s:{s:s, s:s}, s:s}, "		/* insert interface */
		                 "  {s:s, s:s, s:{s:s, s:[s, s]}, s:s},"	/* insert port */
		                 "  {s:s, s:s, s:{s:s, s:[s, s]}, s:s},"	/* insert bridge */
		                 "]}",
		                 "id", call->id,
		                 "method", "transact", "params", "Open_vSwitch",
		                  _expect_bridges (items, priv->db_uuid),
		                  _set_bridges (new_items, priv->db_uuid),
		                  _inc_next_cfg (priv->db_uuid),
		                 "op", "insert", "table", "Interface", "row", "name", call->bridge_iface,
		                 "type", "internal", "uuid-name", "rowIntf",
		                 "op", "insert", "table", "Port", "row", "name", call->bridge_iface,
		                 "interfaces", "named-uuid", "rowIntf", "uuid-name", "rowPort",
		                 "op", "insert", "table", "Bridge", "row", "name", call->bridge_iface,
		                 "ports", "named-uuid", "rowPort", "uuid-name", "rowBridge");
		break;
	case OVSDB_DEL_BR:
		_fill_bridges (self, call->bridge_iface, &items, &new_items);

		msg = json_pack ("{s:i, s:s, s:[s,o,o,o]}",
		                 "id", call->id,
		                 "method", "transact", "params", "Open_vSwitch",
		                 _expect_bridges (items, priv->db_uuid),
		                 _set_bridges (new_items, priv->db_uuid),
		                 _inc_next_cfg (priv->db_uuid));
		break;
	case OVSDB_ADD_PORT:
		_fill_ports (self, call->bridge_iface, call->port_iface, &items, &new_items);
		json_array_append_new (new_items, json_pack ("[s,s]", "named-uuid", "rowPort"));

		msg = json_pack ("{s:i, s:s, s:[s,o,o,o"
		                 "  {s:s, s:s, s:{s:s}, s:s}, "			/* insert interface */
		                 "  {s:s, s:s, s:{s:s, s:[s, s]}, s:s},"	/* insert port */
		                 "]}",
		                 "id", call->id,
		                 "method", "transact", "params", "Open_vSwitch",
		                  _expect_ports (call->bridge_iface, items),
		                  _set_ports (call->bridge_iface, new_items),
		                  _inc_next_cfg (priv->db_uuid),
		                 "op", "insert", "table", "Interface", "row", "name", call->port_iface,
		                 "uuid-name", "rowIntf",
		                 "op", "insert", "table", "Port", "row", "name", call->port_iface,
		                 "interfaces", "named-uuid", "rowIntf", "uuid-name", "rowPort");
		break;
	case OVSDB_DEL_PORT:
		_fill_ports (self, call->bridge_iface, call->port_iface, &items, &new_items);

		msg = json_pack ("{s:i, s:s, s:[s,o,o,o]}",
		                 "id", call->id,
		                 "method", "transact", "params", "Open_vSwitch",
		                  _expect_ports (call->bridge_iface, items),
		                  _set_ports (call->bridge_iface, new_items),
		                  _inc_next_cfg (priv->db_uuid));
		break;
	}

	g_return_if_fail (msg);
	cmd = json_dumps (msg, 0);
	g_string_append (priv->output, cmd);
	json_decref (msg);
	free (cmd);

	ovsdb_write (self);
}

static void
_uuids_to_array (GPtrArray *array, const json_t *items)
{
	const char *key;
	json_t *value;
	size_t index = 0;
	json_t *set_value;
	size_t set_index;

	while (index < json_array_size (items)) {
		key = json_string_value (json_array_get (items, index));
		index++;
		value = json_array_get (items, index);
		index++;

		if (!value)
			return;

		if (g_strcmp0 (key, "uuid") == 0 && json_is_string (value)) {
			g_ptr_array_add (array, g_strdup (json_string_value (value)));
		} else if (g_strcmp0 (key, "set") == 0 && json_is_array (value)) {
			json_array_foreach (value, set_index, set_value) {
				_uuids_to_array (array, set_value);
			}
		}
	}
}

/**
 * ovsdb_got_update:
 *
 * Called when we've got an "update" method call (we asked for it with the monitor
 * command). We use it to maintain a consistent view of bridge list regardless of
 * whether the changes are done by us or externally.
 */
static void
ovsdb_got_update (NMOpenvswitchFactory *self, json_t *msg)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	json_t *ovs = NULL;
	json_t *bridge = NULL;
	json_t *port = NULL;
	json_t *interface = NULL;
	json_t *items;
	json_error_t json_error = { 0, };
	void *iter;
	const char *name;
	const char *key;
	json_t *value;
	OpenvswitchBridge *ovs_bridge;
	OpenvswitchPort *ovs_port;

	if (json_unpack_ex (msg, &json_error, 0, "{s?:o, s?:o, s?:o, s?:o}}",
	                    "Open_vSwitch", &ovs,
	                    "Bridge", &bridge,
	                    "Port", &port,
	                    "Interface", &interface) == -1) {
		/* This doesn't really have to be an error; the key might
		 * be missing if there really are no bridges present. */
		_LOGD ("Bad update: %s", json_error.text);
	}

	if (ovs) {
		iter = json_object_iter (ovs);
		priv->db_uuid = g_strdup (iter ? json_object_iter_key (iter) : NULL);
	}

	json_object_foreach (port, key, value) {
		if (json_unpack (value, "{s:{}}", "old") == 0) {
			_LOGT ("removed a port: %s", name);
			g_hash_table_remove (priv->ports, key);
		}
		if (json_unpack (value, "{s:{s?:s, s?:o}}", "new", "name", &name, "interfaces", &items) == 0) {
			_LOGT ("added a port: %s", name);
			ovs_port = g_slice_new (OpenvswitchPort);
			ovs_port->name = g_strdup (name);
			ovs_port->interfaces = g_ptr_array_new_with_free_func (g_free);
			_uuids_to_array (ovs_port->interfaces, items);
			g_hash_table_insert (priv->ports, g_strdup (key), ovs_port);
		}
	}

	json_object_foreach (bridge, key, value) {
		if (json_unpack (value, "{s:{}}", "old") == 0) {
			_LOGT ("removed a bridge: %s", name);
			g_hash_table_remove (priv->bridges, key);
		}
		if (json_unpack (value, "{s:{s?:s, s?:o}}", "new", "name", &name, "ports", &items) == 0) {
			_LOGT ("added a bridge: %s", name);
			ovs_bridge = g_slice_new (OpenvswitchBridge);
			ovs_bridge->name = g_strdup (name);
			ovs_bridge->ports = g_ptr_array_new_with_free_func (g_free);
			_uuids_to_array (ovs_bridge->ports, items);
			g_hash_table_insert (priv->bridges, g_strdup (key), ovs_bridge);
		}
	}
}

/**
 * ovsdb_got_msg::
 *
 * Called when when a complete JSON object was seen and unmarshalled.
 * Either finishes a method call or processes a method call.
 */
static void
ovsdb_got_msg (NMOpenvswitchFactory *self, json_t *msg)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	json_t *json_id = NULL;
	guint64 id;
	json_t *json_method = NULL;
	const char *method = NULL;
	OvsdbMethodCall *call = NULL;
	json_error_t json_error = { 0, };
	json_t *params = NULL;

	json_id = json_object_get (msg, "id");
	if (json_is_number (json_id)) {
		/* This is a response to a method call. */
		id = json_integer_value (json_id);

		if (!priv->calls->len) {
			_LOGE ("there are no queued calls expecting response %ld", id);
			ovsdb_disconnect (self);
			return;
		}
		call = &g_array_index (priv->calls, OvsdbMethodCall, 0);
		if (call->id != id) {
			_LOGE ("expected a response to call %ld, not %ld", call->id, id);
			ovsdb_disconnect (self);
			return;
		}

		/* Cool, we found a corresponsing call. Finish it. */
		call->callback (self, msg, NULL, call->user_data);
		g_array_remove_index (priv->calls, 0);

		/* Now we're free to serialize and send the next command, if any. */
		ovsdb_next_command (self);
		return;
	}

	json_method = json_object_get (msg, "method");
	if (json_is_string (json_method))
		method = json_string_value (json_method);
	if (g_strcmp0 (method, "update") == 0) {
		/* This is a update method call. */
		if (json_unpack_ex (msg, &json_error, 0, "{s:[n,o]}",
		                    "params", &params) == -1) {
			_LOGD ("a update call with no params: %s", json_error.text);
			ovsdb_disconnect (self);
			return;
		}
		ovsdb_got_update (self, params);
		return;
	}

	/* This is a message we are not interested in. */
	_LOGD ("got an unknown message, ignoring");
}

/*****************************************************************************/

/* Lower level marshalling and demarshalling of the JSON-RPC traffic on the
 * ovsdb socket. */

static size_t
_json_callback (void *buffer, size_t buflen, void *user_data)
{
	NMOpenvswitchFactory *self = NM_OPENVSWITCH_FACTORY (user_data);
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);

	if (priv->bufp == priv->input->len) {
		/* No more bytes buffered for decoding. */
		return 0;
	}

	/* Pass one more byte to the JSON decoder. */
	*(char *)buffer = priv->input->str[priv->bufp];
	priv->bufp++;

	return (size_t)1;
}

/**
 * ovsdb_read_cb:
 *
 * Read out the data available from the ovsdb socket and try to deserialize
 * the JSON. If we see a complete object, pass it upwards to ovsdb_got_msg().
 */
static void
ovsdb_read_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	NMOpenvswitchFactory *self = NM_OPENVSWITCH_FACTORY (user_data);
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	GInputStream *stream = G_INPUT_STREAM (source_object);
	GError *error = NULL;
	gssize size;
	json_t *msg;
	json_error_t json_error = { 0, };

	size = g_input_stream_read_finish (stream, res, &error);
	if (size == -1) {
		_LOGW ("short read from ovsdb: %s", error->message);
		g_clear_error (&error);
		ovsdb_disconnect (self);
		return;
	}

	g_string_append_len (priv->input, priv->buf, size);
	do {
		priv->bufp = 0;
		/* The callback always eats up only up to a single byte. This makes
		 * it possible for us to identify complete JSON objects in spite of
		 * us not knowing the length in advance. */
		msg = json_load_callback (_json_callback, self, JSON_DISABLE_EOF_CHECK, &json_error);
		if (msg) {
			ovsdb_got_msg (self, msg);
			g_string_erase (priv->input, 0, priv->bufp);
		}
		json_decref (msg);
	} while (msg);

	if (size)
		ovsdb_read (self);
}

static void
ovsdb_read (NMOpenvswitchFactory *self)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);

	g_input_stream_read_async (g_io_stream_get_input_stream (G_IO_STREAM (priv->conn)),
		priv->buf, sizeof(priv->buf),
		G_PRIORITY_DEFAULT, NULL, ovsdb_read_cb, self);
}

static void
ovsdb_write_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GOutputStream *stream = G_OUTPUT_STREAM (source_object);
	NMOpenvswitchFactory *self = NM_OPENVSWITCH_FACTORY (user_data);
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	GError *error = NULL;
	gssize size;

	size = g_output_stream_write_finish (stream, res, &error);
	if (size == -1) {
		_LOGW ("short write to ovsdb: %s", error->message);
		g_clear_error (&error);
		ovsdb_disconnect (self);
		return;
	}

	g_string_erase (priv->output, 0, size);

	ovsdb_write (self);
}

static void
ovsdb_write (NMOpenvswitchFactory *self)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	GOutputStream *stream;

	if (!priv->output->len)
		return;

	stream = g_io_stream_get_output_stream (G_IO_STREAM (priv->conn));
	if (g_output_stream_has_pending (stream))
		return;

	g_output_stream_write_async (stream,
	                             priv->output->str, priv->output->len,
	                             G_PRIORITY_DEFAULT, NULL, ovsdb_write_cb, self);
}
/*****************************************************************************/

/* Routines to maintain the ovsdb connection. */

/**
 * ovsdb_disconnect:
 *
 * Clean up the internal state to the point equivalent to before connecting.
 * Apart from clean shutdown this is a good response to unexpected trouble,
 * since the next method call attempt a will trigger reconnect which hopefully
 * puts us back in sync.
 */
static void
ovsdb_disconnect (NMOpenvswitchFactory *self)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	OvsdbMethodCall *call;
	GError *error;

	_LOGD ("disconnecting from ovsdb");

	while (priv->calls->len) {
		error = NULL;
		call = &g_array_index (priv->calls, OvsdbMethodCall, priv->calls->len - 1);
		g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
		call->callback (self, NULL, error, call->user_data);
		g_array_remove_index (priv->calls, priv->calls->len - 1);
	}

	g_string_truncate (priv->input, 0);
	g_string_truncate (priv->output, 0);
	g_clear_object (&priv->client);
	g_clear_object (&priv->conn);
	g_clear_pointer (&priv->db_uuid, g_free);
}

static void
_monitor_bridges_cb (NMOpenvswitchFactory *self, json_t *response, GError *error, gpointer user_data)
{
	json_t *result = NULL;
	json_error_t json_error = { 0, };

	if (error) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			_LOGI ("%s", error->message);
			ovsdb_disconnect (self);
		}

		g_clear_error (&error);
		return;
	}

	if (json_unpack_ex (response, &json_error, 0, "{s:o}",
	                    "result", &result) == -1) {
		_LOGW ("monitor_bridges finished with no result: %s", json_error.text);
		ovsdb_disconnect (self);
		return;
	}

	/* Treat the first response the same as the subsequent "update"
	 * messages we eventually get. */
	ovsdb_got_update (self, result);
}

static void
_client_connect_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GSocketClient *client = G_SOCKET_CLIENT (source_object);
	NMOpenvswitchFactory *self = NM_OPENVSWITCH_FACTORY (user_data);
	NMOpenvswitchFactoryPrivate *priv;
	GError *error = NULL;
	GSocketConnection *conn;

	conn = g_socket_client_connect_finish (client, res, &error);
	if (conn == NULL) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			_LOGI ("%s", error->message);

		ovsdb_disconnect (self);
		g_clear_error (&error);
		return;
	}

	priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	priv->conn = conn;
	g_clear_object (&priv->cancellable);

	ovsdb_read (self);
	ovsdb_next_command (self);
}

/**
 * ovsdb_try_connect:
 *
 * Establish a connection to ovsdb unless it's already established or being
 * established. Queues a monitor command as a very first one so that we're in
 * sync when other commands are issued.
 */
static void
ovsdb_try_connect (NMOpenvswitchFactory *self)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);
	GSocketAddress *addr;

	if (priv->client)
		return;

	/* XXX: This should probably be made configurable via NetworkManager.conf */
	addr = g_unix_socket_address_new (RUNSTATEDIR "/openvswitch/db.sock");

	priv->client = g_socket_client_new ();
	priv->cancellable = g_cancellable_new ();
	g_socket_client_connect_async (priv->client, G_SOCKET_CONNECTABLE (addr),
	                               priv->cancellable, _client_connect_cb, self);
	g_object_unref (addr);

	/* Queue a monitor call before any other command, ensuring that we have an up
	 * to date view of existing bridged that we need for add and remove ops. */
	ovsdb_call_method (self, OVSDB_MONITOR, NULL, NULL, _monitor_bridges_cb, NULL);
}

/*****************************************************************************/

/* Public functions useful for NMDeviceOpenvswitch to maintain the life cycle of
 * their ovsdb entries without having to deal with ovsdb complexities themselves. */

typedef struct {
	NMOpenvswitchFactoryCallback callback;
	gpointer user_data;
} OpenvswitchFactoryCall;

static void
_transact_cb (NMOpenvswitchFactory *self, json_t *response, GError *error, gpointer user_data)
{
	OpenvswitchFactoryCall *call = user_data;
	json_error_t json_error = { 0, };
	const char *err;
	const char *err_details;
	json_t *result;
	size_t index;
	json_t *value;

	if (error)
		goto out;

	if (json_unpack_ex (response, &json_error, 0, "{s:o}", "result", &result) == -1) {
		g_set_error (&error, NM_DEVICE_ERROR, NM_DEVICE_ERROR_CREATION_FAILED,
		             "Bad response from ovsdb: %s", json_error.text);
		goto out;
	}

	json_array_foreach (result, index, value) {
		if (json_unpack (value, "{s:s, s:s}", "error", &err, "details", &err_details) == 0) {
			g_set_error (&error, NM_DEVICE_ERROR, NM_DEVICE_ERROR_CREATION_FAILED,
			             "Error running the transaction: %s: %s", err, err_details);
			goto out;
		}
	}

out:
	call->callback (error, call->user_data);
	g_slice_free (OpenvswitchFactoryCall, call);
}

static void
_transact_call (NMOpenvswitchFactory *self, OvsdbCommand command,
              const char *bridge_iface, const char *port_iface,
              NMOpenvswitchFactoryCallback callback, gpointer user_data)
{
	OpenvswitchFactoryCall *call;

	call = g_slice_new (OpenvswitchFactoryCall);
	call->callback = callback;
	call->user_data = user_data;

	ovsdb_call_method (self, command, bridge_iface, port_iface, _transact_cb, call);
}

void
nm_openvswitch_factory_add_br (NMOpenvswitchFactory *self, const char *bridge_iface,
                               NMOpenvswitchFactoryCallback callback, gpointer user_data)
{
	_transact_call (self, OVSDB_ADD_BR, bridge_iface, NULL,
	                callback, user_data);
}

void
nm_openvswitch_factory_del_br (NMOpenvswitchFactory *self, const char *bridge_iface,
                               NMOpenvswitchFactoryCallback callback, gpointer user_data)
{
	_transact_call (self, OVSDB_DEL_BR, bridge_iface, NULL,
	                callback, user_data);
}

void
nm_openvswitch_factory_add_port (NMOpenvswitchFactory *self,
                                 const char *bridge_iface, const char *port_iface,
                                 NMOpenvswitchFactoryCallback callback, gpointer user_data)
{
	_transact_call (self, OVSDB_ADD_PORT, bridge_iface, port_iface,
	                callback, user_data);
}

void
nm_openvswitch_factory_del_port (NMOpenvswitchFactory *self,
                                 const char *bridge_iface, const char *port_iface,
                                 NMOpenvswitchFactoryCallback callback, gpointer user_data)
{
	_transact_call (self, OVSDB_DEL_PORT, bridge_iface, port_iface,
	                callback, user_data);
}

/*****************************************************************************/

NM_DEVICE_FACTORY_DECLARE_TYPES (
	NM_DEVICE_FACTORY_DECLARE_LINK_TYPES    (NM_LINK_TYPE_OPENVSWITCH)
	NM_DEVICE_FACTORY_DECLARE_SETTING_TYPES (NM_SETTING_OPENVSWITCH_SETTING_NAME)
)

G_MODULE_EXPORT NMDeviceFactory *
nm_device_factory_create (GError **error)
{
	return (NMDeviceFactory *) g_object_new (NM_TYPE_OPENVSWITCH_FACTORY, NULL);
}

static void
start (NMDeviceFactory *factory)
{
	NMOpenvswitchFactory *self = NM_OPENVSWITCH_FACTORY (factory);

	ovsdb_try_connect (self);
}

static NMDevice *
create_device (NMDeviceFactory *factory,
               const char *iface,
               const NMPlatformLink *plink,
               NMConnection *connection,
               gboolean *out_ignore)
{
	if (g_strcmp0 (iface, "ovs-system") == 0)
		return NULL;

	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_OPENVSWITCH,
	                                  NM_DEVICE_IFACE, iface,
	                                  NM_DEVICE_TYPE_DESC, "OpenVSwitch",
	                                  NM_DEVICE_DEVICE_TYPE, NM_DEVICE_TYPE_OPENVSWITCH,
	                                  NM_DEVICE_LINK_TYPE, NM_LINK_TYPE_OPENVSWITCH,
	                                  NULL);
}

static void
_free_port (gpointer data)
{
	OpenvswitchPort *ovs_port = data;

	g_ptr_array_free (ovs_port->interfaces, TRUE);
	g_slice_free (OpenvswitchPort, ovs_port);
}

static void
_free_bridge (gpointer data)
{
	OpenvswitchBridge *ovs_bridge = data;

	g_ptr_array_free (ovs_bridge->ports, TRUE);
	g_slice_free (OpenvswitchBridge, ovs_bridge);
}

static void
nm_openvswitch_factory_init (NMOpenvswitchFactory *self)
{
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);

	priv->calls = g_array_new (FALSE, TRUE, sizeof (OvsdbMethodCall));
	priv->input = g_string_new (NULL);
	priv->output = g_string_new (NULL);
	priv->ports = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, _free_port);
	priv->bridges = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, _free_bridge);
}

static void
dispose (GObject *object)
{
	NMOpenvswitchFactory *self = NM_OPENVSWITCH_FACTORY (object);
	NMOpenvswitchFactoryPrivate *priv = NM_OPENVSWITCH_FACTORY_GET_PRIVATE (self);

	ovsdb_disconnect (self);

	g_string_free (priv->input, TRUE);
	priv->input = NULL;
	g_string_free (priv->output, TRUE);
	priv->output = NULL;

	g_array_free (priv->calls, TRUE);
	priv->calls = NULL;

	g_clear_pointer (&priv->ports, g_hash_table_destroy);
	g_clear_pointer (&priv->bridges, g_hash_table_destroy);

	g_cancellable_cancel (priv->cancellable);
	g_clear_object (&priv->cancellable);

	/* Chain up to the parent class */
	G_OBJECT_CLASS (nm_openvswitch_factory_parent_class)->dispose (object);
}

static void
nm_openvswitch_factory_class_init (NMOpenvswitchFactoryClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMDeviceFactoryClass *factory_class = NM_DEVICE_FACTORY_CLASS (klass);

	object_class->dispose = dispose;

	factory_class->get_supported_types = get_supported_types;
	factory_class->start = start;
	factory_class->create_device = create_device;
}
