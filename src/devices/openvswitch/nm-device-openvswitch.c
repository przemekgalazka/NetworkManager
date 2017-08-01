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
 * Copyright 2017 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-device-openvswitch.h"
#include "nm-openvswitch-factory.h"

#include "devices/nm-device-private.h"
#include "nm-setting-openvswitch.h"

#include "introspection/org.freedesktop.NetworkManager.Device.Openvswitch.h"

#include "devices/nm-device-logging.h"
_LOG_DECLARE_SELF(NMDeviceOpenvswitch);

/*****************************************************************************/

struct _NMDeviceOpenvswitch {
	NMDevice parent;
};

struct _NMDeviceOpenvswitchClass {
	NMDeviceClass parent;
};

G_DEFINE_TYPE (NMDeviceOpenvswitch, nm_device_openvswitch, NM_TYPE_DEVICE)

/*****************************************************************************/

static void
link_changed (NMDevice *device, const NMPlatformLink *pllink)
{
	NMDeviceOpenvswitch *self = NM_DEVICE_OPENVSWITCH (device);

	NM_DEVICE_CLASS (nm_device_openvswitch_parent_class)->link_changed (device, pllink);

	if (pllink && nm_device_get_state (device) == NM_DEVICE_STATE_CONFIG) {
		_LOGD (LOGD_DEVICE, "the link appeared, continuing activation");
		nm_device_activate_schedule_stage2_device_config (device);
	}
}

static void
add_br_cb (GError *error, gpointer user_data)
{
	NMDeviceOpenvswitch *self = user_data;

	if (error) {
		_LOGW (LOGD_DEVICE, "%s", error->message);
		nm_device_state_changed (NM_DEVICE (self),
		                         NM_DEVICE_STATE_FAILED,
		                         NM_DEVICE_STATE_REASON_UNKNOWN);
	}

	g_object_unref (self);
}

static gboolean
create_and_realize (NMDevice *device,
                    NMConnection *connection,
                    NMDevice *parent,
                    const NMPlatformLink **out_plink,
                    GError **error)
{
	NMDeviceFactory *factory;

	factory = nm_device_factory_manager_find_factory_for_link_type (NM_LINK_TYPE_OPENVSWITCH);
	g_return_val_if_fail (factory, FALSE);

	nm_openvswitch_factory_add_br (NM_OPENVSWITCH_FACTORY (factory),
	                               nm_device_get_iface (device),
	                               add_br_cb,
	                               g_object_ref (device));

	/* We don't have a plink yet, since the device is eventually instantiated
	 * by ovs-vswitchd asynchronously. Manager knows and manager is fine with that. */

	return TRUE;
}

static void
del_br_cb (GError *error, gpointer user_data)
{
	NMDeviceOpenvswitch *self = user_data;

	if (error) {
	        _LOGW (LOGD_DEVICE, "%s", error->message);
	        nm_device_state_changed (NM_DEVICE (self),
	                                 NM_DEVICE_STATE_FAILED,
	                                 NM_DEVICE_STATE_REASON_UNKNOWN);
	}

	g_object_unref (self);
}

static gboolean
unrealize (NMDevice *device, GError **error)
{
	NMDeviceFactory *factory;

	factory = nm_device_factory_manager_find_factory_for_link_type (NM_LINK_TYPE_OPENVSWITCH);
	g_return_val_if_fail (factory, FALSE);

	nm_openvswitch_factory_del_br (NM_OPENVSWITCH_FACTORY (factory),
	                               nm_device_get_iface (device),
	                               del_br_cb,
	                               g_object_ref (device));

	return TRUE;
}

static NMDeviceCapabilities
get_generic_capabilities (NMDevice *device)
{
	return NM_DEVICE_CAP_CARRIER_DETECT | NM_DEVICE_CAP_IS_SOFTWARE;
}


static gboolean
check_connection_compatible (NMDevice *device, NMConnection *connection)
{
	NMSettingOpenvswitch *s_openvswitch;

	if (!NM_DEVICE_CLASS (nm_device_openvswitch_parent_class)->check_connection_compatible (device, connection))
		return FALSE;

	s_openvswitch = nm_connection_get_setting_openvswitch (connection);
	if (!s_openvswitch)
		return FALSE;

	return TRUE;
}

static NMActStageReturn
act_stage2_config (NMDevice *device, NMDeviceStateReason *out_failure_reason)
{
	NMDeviceOpenvswitch *self = NM_DEVICE_OPENVSWITCH (device);

	if (nm_device_get_ifindex (device)) {
		return NM_ACT_STAGE_RETURN_SUCCESS;
	} else {
		_LOGD (LOGD_DEVICE, "the link is not there, waiting for it to appear");
		return NM_ACT_STAGE_RETURN_POSTPONE;
	}
}

static void
update_connection (NMDevice *device, NMConnection *connection)
{
	NMSettingOpenvswitch *s_openvswitch = nm_connection_get_setting_openvswitch (connection);

	if (!s_openvswitch) {
		s_openvswitch = (NMSettingOpenvswitch *) nm_setting_openvswitch_new ();
		nm_connection_add_setting (connection, (NMSetting *) s_openvswitch);
	}
}

static void
add_port_cb (GError *error, gpointer user_data)
{
	NMDevice *slave = user_data;

	if (error) {
	        nm_log_warn (LOGD_DEVICE, "device %s could not be added to a ovs port: %s",
		             nm_device_get_iface (slave), error->message);
	        nm_device_state_changed (slave,
	                                 NM_DEVICE_STATE_FAILED,
	                                 NM_DEVICE_STATE_REASON_UNKNOWN);
	}

	g_object_unref (slave);
}

static gboolean
enslave_slave (NMDevice *device, NMDevice *slave, NMConnection *connection, gboolean configure)
{
	NMDeviceFactory *factory;

	factory = nm_device_factory_manager_find_factory_for_link_type (NM_LINK_TYPE_OPENVSWITCH);
	g_return_val_if_fail (factory, FALSE);

	if (configure) {
		nm_openvswitch_factory_add_port (NM_OPENVSWITCH_FACTORY (factory),
		                                 nm_device_get_iface (device),
		                                 nm_device_get_iface (slave),
		                                 add_port_cb,
		                                 g_object_ref (slave));
	}

	return TRUE;
}

static void
del_port_cb (GError *error, gpointer user_data)
{
	NMDevice *slave = user_data;

	if (error) {
	        nm_log_warn (LOGD_DEVICE, "device %s could not be removed from a ovs port: %s",
		             nm_device_get_iface (slave), error->message);
	        nm_device_state_changed (slave,
	                                 NM_DEVICE_STATE_FAILED,
	                                 NM_DEVICE_STATE_REASON_UNKNOWN);
	}

	g_object_unref (slave);
}

static void
release_slave (NMDevice *device, NMDevice *slave, gboolean configure)
{
	NMDeviceFactory *factory;

	factory = nm_device_factory_manager_find_factory_for_link_type (NM_LINK_TYPE_OPENVSWITCH);
	g_return_if_fail (factory);

	if (configure) {
		nm_openvswitch_factory_del_port (NM_OPENVSWITCH_FACTORY (factory),
		                                 nm_device_get_iface (device),
		                                 nm_device_get_iface (slave),
		                                 del_port_cb,
		                                 g_object_ref (slave));
	}
}

/*****************************************************************************/

static void
nm_device_openvswitch_init (NMDeviceOpenvswitch *self)
{
}

static void
nm_device_openvswitch_class_init (NMDeviceOpenvswitchClass *klass)
{
	NMDeviceClass *device_class = NM_DEVICE_CLASS (klass);

	NM_DEVICE_CLASS_DECLARE_TYPES (klass, NULL, NM_LINK_TYPE_OPENVSWITCH)

	device_class->connection_type = NM_SETTING_OPENVSWITCH_SETTING_NAME;
	device_class->is_master = TRUE;

	device_class->link_changed = link_changed;
	device_class->create_and_realize = create_and_realize;
	device_class->unrealize = unrealize;
	device_class->get_generic_capabilities = get_generic_capabilities;
	device_class->check_connection_compatible = check_connection_compatible;
	device_class->act_stage2_config = act_stage2_config;
	device_class->update_connection = update_connection;
	device_class->enslave_slave = enslave_slave;
	device_class->release_slave = release_slave;

	nm_exported_object_class_add_interface (NM_EXPORTED_OBJECT_CLASS (klass),
	                                        NMDBUS_TYPE_DEVICE_OPENVSWITCH_SKELETON,
	                                        NULL);
}
