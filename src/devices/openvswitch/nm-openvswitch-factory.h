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

#ifndef __NETWORKMANAGER_OPENVSWITCH_FACTORY_H__
#define __NETWORKMANAGER_OPENVSWITCH_FACTORY_H__

#include "devices/nm-device-factory.h"

#define NM_TYPE_OPENVSWITCH_FACTORY            (nm_openvswitch_factory_get_type ())
#define NM_OPENVSWITCH_FACTORY(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_OPENVSWITCH_FACTORY, NMOpenvswitchFactory))
#define NM_OPENVSWITCH_FACTORY_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), NM_TYPE_OPENVSWITCH_FACTORY, NMOpenvswitchFactoryClass))
#define NM_IS_OPENVSWITCH_FACTORY(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_OPENVSWITCH_FACTORY))
#define NM_IS_OPENVSWITCH_FACTORY_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NM_TYPE_OPENVSWITCH_FACTORY))
#define NM_OPENVSWITCH_FACTORY_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), NM_TYPE_OPENVSWITCH_FACTORY, NMOpenvswitchFactoryClass))

typedef struct _NMOpenvswitchFactory NMOpenvswitchFactory;
typedef struct _NMOpenvswitchFactoryClass NMOpenvswitchFactoryClass;

typedef void (*NMOpenvswitchFactoryCallback)(GError *error, gpointer user_data);

GType nm_openvswitch_factory_get_type (void);

void nm_openvswitch_factory_add_br (NMOpenvswitchFactory *self, const char *iface,
                                    NMOpenvswitchFactoryCallback callback, gpointer user_data);

void nm_openvswitch_factory_del_br (NMOpenvswitchFactory *self, const char *iface,
                                    NMOpenvswitchFactoryCallback callback, gpointer user_data);

void nm_openvswitch_factory_add_port (NMOpenvswitchFactory *self,
                                      const char *bridge_iface, const char *port_iface,
                                      NMOpenvswitchFactoryCallback callback, gpointer user_data);

void nm_openvswitch_factory_del_port (NMOpenvswitchFactory *self,
                                      const char *bridge_iface, const char *port_iface,
                                      NMOpenvswitchFactoryCallback callback, gpointer user_data);

#endif /* __NETWORKMANAGER_OPENVSWITCH_FACTORY_H__ */
