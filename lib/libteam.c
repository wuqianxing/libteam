/*
 *   libteam.c - Network team device driver library
 *   Copyright (C) 2011 Jiri Pirko <jpirko@redhat.com>
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <netlink/cli/utils.h>
#include <netlink/cli/link.h>
#include <linux/if_team.h>
#include <linux/types.h>
#include <team.h>
#include "list.h"

/**
 * SECTION: team_handler
 * @short_description: libteam context
 */

struct team_handle {
	struct nl_sock *	nl_sock;
	int			nl_sock_err;
	struct nl_sock *	nl_sock_event;
	int			family;
	uint32_t		ifindex;
	struct list_item	port_list;
	struct list_item	option_list;
	struct {
		struct list_item		list;
		team_change_type_mask_t		pending_type_mask;
	} change_handler;
	struct {
		struct nl_sock *	sock;
		struct nl_cache *	link_cache;
	} nl_cli;
	void (*log_fn)(struct team_handle *th, int priority,
		       const char *file, int line, const char *fn,
		       const char *format, va_list args);
	int log_priority;
};

/**
 * SECTION: logging
 * @short_description: libteam logging facility
 */

static inline void __attribute__((always_inline, format(printf, 2, 3)))
team_log_null(struct team_handle *th, const char *format, ...) {}

#define team_log_cond(th, prio, arg...)					\
	do {								\
		if (team_get_log_priority(th) >= prio)			\
			team_log(th, prio, __FILE__, __LINE__,		\
				 __FUNCTION__, ## arg);			\
	} while (0)

#ifdef ENABLE_LOGGING
#  ifdef ENABLE_DEBUG
#    define dbg(th, arg...) team_log_cond(th, LOG_DEBUG, ## arg)
#  else
#    define dbg(th, arg...) team_log_null(th, ## arg)
#  endif
#  define info(th, arg...) team_log_cond(th, LOG_INFO, ## arg)
#  define err(th, arg...) team_log_cond(th, LOG_ERR, ## arg)
#else
#  define dbg(th, arg...) team_log_null(th, ## arg)
#  define info(th, arg...) team_log_null(th, ## arg)
#  define err(th, arg...) team_log_null(th, ## arg)
#endif

#define TEAM_EXPORT __attribute__ ((visibility("default")))

static void team_log(struct team_handle *th, int priority,
		     const char *file, int line, const char *fn,
		     const char *format, ...)
{
	va_list args;

	va_start(args, format);
	th->log_fn(th, priority, file, line, fn, format, args);
	va_end(args);
}

static void log_stderr(struct team_handle *th, int priority,
		       const char *file, int line, const char *fn,
		       const char *format, va_list args)
{
	fprintf(stderr, "libteam: %s: ", fn);
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}

static int log_priority(const char *priority)
{
	char *endptr;
	int prio;

	prio = strtol(priority, &endptr, 10);
	if (endptr[0] == '\0' || isspace(endptr[0]))
		return prio;
	if (strncmp(priority, "err", 3) == 0)
		return LOG_ERR;
	if (strncmp(priority, "info", 4) == 0)
		return LOG_INFO;
	if (strncmp(priority, "debug", 5) == 0)
		return LOG_DEBUG;
	return 0;
}

/**
 * SECTION: libnl helpers
 * @short_description: various libnl helper functions
 */

static int nl2syserr(int nl_error)
{
	switch (abs(nl_error)) {
	case 0:				return 0;
	case NLE_EXIST:			return EEXIST;
	case NLE_NOADDR:		return EADDRNOTAVAIL;
	case NLE_OBJ_NOTFOUND:		return ENOENT;
	case NLE_INTR:			return EINTR;
	case NLE_AGAIN:			return EAGAIN;
	case NLE_BAD_SOCK:		return ENOTSOCK;
	case NLE_NOACCESS:		return EACCES;
	case NLE_INVAL:			return EINVAL;
	case NLE_NOMEM:			return ENOMEM;
	case NLE_AF_NOSUPPORT:		return EAFNOSUPPORT;
	case NLE_PROTO_MISMATCH:	return EPROTONOSUPPORT;
	case NLE_OPNOTSUPP:		return EOPNOTSUPP;
	case NLE_PERM:			return EPERM;
	case NLE_BUSY:			return EBUSY;
	case NLE_RANGE:			return ERANGE;
	case NLE_NODEV:			return ENODEV;
	default:			return EINVAL;
	}
}

/**
 * SECTION: Netlink helpers
 * @short_description: Various netlink helpers
 */

static int send_and_recv(struct team_handle *th, struct nl_msg *msg,
			 int (*valid_handler)(struct nl_msg *, void *),
			 void *valid_data)
{
	int err = -ENOMEM;

	err = nl_send_auto_complete(th->nl_sock, msg);
	if (err < 0) {
		err = -nl2syserr(err);
		goto out;
	}

	th->nl_sock_err = 1;

	if (valid_handler)
		nl_socket_modify_cb(th->nl_sock, NL_CB_VALID, NL_CB_CUSTOM,
				    valid_handler, valid_data);

	while (th->nl_sock_err > 0)
		nl_recvmsgs_default(th->nl_sock);

	err = th->nl_sock_err;

 out:
	nlmsg_free(msg);
	return err;
}

static int ack_handler(struct nl_msg *msg, void *arg)
{
	int *err = arg;

	*err = 0;
	return NL_STOP;
}

static int finish_handler(struct nl_msg *msg, void *arg)
{
	int *err = arg;

	*err = 0;
	return NL_SKIP;
}

static int error_handler(struct sockaddr_nl *nla, struct nlmsgerr *nl_err,
			 void *arg)
{
	int *err = arg;

	*err = nl_err->error;
	return NL_SKIP;
}

static int cli_cache_refill(struct team_handle *th)
{
	return nl_cache_refill(th->nl_cli.sock, th->nl_cli.link_cache);
}

/**
 * SECTION: Change handlers
 * @short_description: event change handlers handling
 */

struct change_handler_item {
	struct list_item		list;
	struct team_change_handler *	handler;
};

static void set_call_change_handlers(struct team_handle *th,
				     team_change_type_mask_t set_type_mask)
{
	th->change_handler.pending_type_mask |= set_type_mask;
}

static void check_call_change_handlers(struct team_handle *th,
				       team_change_type_mask_t call_type_mask)
{
	struct change_handler_item *handler_item;
	team_change_type_mask_t to_call_type_mask =
			th->change_handler.pending_type_mask & call_type_mask;

	list_for_each_node_entry(handler_item, &th->change_handler.list, list) {
		struct team_change_handler *handler = handler_item->handler;
		team_change_type_mask_t item_type_mask =
				handler->type_mask & to_call_type_mask;

		if (item_type_mask) {
			handler->func(th, handler->func_priv, item_type_mask);
		}
	}
	th->change_handler.pending_type_mask &= ~call_type_mask;
}

static struct change_handler_item *
find_change_handler(struct team_handle *th,
		    struct team_change_handler *handler)
{
	struct change_handler_item *handler_item;

	list_for_each_node_entry(handler_item, &th->change_handler.list, list)
		if (handler_item->handler == handler)
			return handler_item;
	return NULL;
}

/**
 * team_change_handler_register:
 * @th: libteam library context
 * @handler: event handler structure
 *
 * Registers custom @handler structure which defines a function which
 * going to be called on defined events.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_change_handler_register(struct team_handle *th,
				 struct team_change_handler *handler)
{
	struct change_handler_item *handler_item;

	if (find_change_handler(th, handler))
		return -EEXIST;
	handler_item = malloc(sizeof(struct change_handler_item));
	if (!handler_item)
		return -ENOMEM;
	handler_item->handler = handler;
	list_add(&th->change_handler.list, &handler_item->list);
	return 0;
}

/**
 * team_change_handler_unregister:
 * @th: libteam library context
 * @handler: event handler structure
 *
 * Unregisters custom @handler structure.
 *
 **/
TEAM_EXPORT
void team_change_handler_unregister(struct team_handle *th,
				    struct team_change_handler *handler)
{
	struct change_handler_item *handler_item;

	handler_item = find_change_handler(th, handler);
	if (!handler_item)
		return;
	list_del(&handler_item->list);
	free(handler_item);
}

/**
 * SECTION: Ports
 * @short_description: port getters, port_list manipulators
 */

struct team_port {
	struct list_item	list;
	uint32_t		ifindex;
	uint32_t		speed;
	uint8_t			duplex;
	bool			linkup;
	bool			changed;
};

static void flush_port_list(struct team_handle *th)
{
	struct team_port *port, *tmp;

	list_for_each_node_entry_safe(port, tmp, &th->port_list, list) {
		list_del(&port->list);
		free(port);
	}
}

static int get_port_list_handler(struct nl_msg *msg, void *arg)
{
	struct nlmsghdr *nlh = nlmsg_hdr(msg);
	struct team_handle *th = arg;
	struct nlattr *attrs[TEAM_ATTR_MAX + 1];
	struct nlattr *nl_port;
	struct nlattr *port_attrs[TEAM_ATTR_PORT_MAX + 1];
	int i;
	uint32_t team_ifindex = 0;
	struct list_item tmp_list;

	list_init(&tmp_list);
	genlmsg_parse(nlh, 0, attrs, TEAM_ATTR_MAX, NULL);
	if (attrs[TEAM_ATTR_TEAM_IFINDEX])
		team_ifindex = nla_get_u32(attrs[TEAM_ATTR_TEAM_IFINDEX]);

	if (team_ifindex != th->ifindex)
		return NL_SKIP;

	if (!attrs[TEAM_ATTR_LIST_PORT])
		return NL_SKIP;

	nla_for_each_nested(nl_port, attrs[TEAM_ATTR_LIST_PORT], i) {
		struct team_port *port;

		if (nla_parse_nested(port_attrs, TEAM_ATTR_PORT_MAX,
				     nl_port, NULL)) {
			err(th, "Failed to parse nested attributes.");
			return NL_SKIP;
		}

		if (!port_attrs[TEAM_ATTR_PORT_IFINDEX]) {
			err(th, "ifindex port attribute not found.");
			return NL_SKIP;
		}
		port = malloc(sizeof(struct team_port));
		if (!port) {
			err(th, "Malloc failed.");
			return NL_SKIP;
		}
		memset(port, 0, sizeof(struct team_port));
		port->ifindex = nla_get_u32(port_attrs[TEAM_ATTR_PORT_IFINDEX]);
		if (port_attrs[TEAM_ATTR_PORT_CHANGED])
			port->changed = true;
		if (port_attrs[TEAM_ATTR_PORT_LINKUP])
			port->linkup = true;
		if (port_attrs[TEAM_ATTR_PORT_SPEED])
			port->speed = nla_get_u32(port_attrs[TEAM_ATTR_PORT_SPEED]);
		if (port_attrs[TEAM_ATTR_PORT_DUPLEX])
			port->duplex = nla_get_u8(port_attrs[TEAM_ATTR_PORT_DUPLEX]);

		list_add(&tmp_list, &port->list);
	}

	flush_port_list(th);
	list_move_nodes(&th->port_list, &tmp_list);

	set_call_change_handlers(th, TEAM_PORT_CHANGE);
	return NL_SKIP;
}

static int get_port_list(struct team_handle *th)
{
	struct nl_msg *msg;
	int err;

	msg = nlmsg_alloc();
	if (!msg)
		return -ENOMEM;

	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, th->family, 0, 0,
			 TEAM_CMD_PORT_LIST_GET, 0);
	NLA_PUT_U32(msg, TEAM_ATTR_TEAM_IFINDEX, th->ifindex);

	err = send_and_recv(th, msg, get_port_list_handler, th);
	if (err)
		return err;

	check_call_change_handlers(th, TEAM_PORT_CHANGE);
	return 0;

nla_put_failure:
	nlmsg_free(msg);
	return -ENOBUFS;
}

/**
 * team_get_next_port:
 * @th: libteam library context
 * @port: port structure
 *
 * Get next port in list.
 *
 * Returns: port next to @port passed.
 **/
TEAM_EXPORT
struct team_port *team_get_next_port(struct team_handle *th,
				     struct team_port *port)
{
	return list_get_next_node_entry(&th->port_list, port, list);
}

/**
 * team_get_port_ifindex:
 * @port: port structure
 *
 * Get port interface index.
 *
 * Returns: port interface index as idenfified by in kernel.
 **/
TEAM_EXPORT
uint32_t team_get_port_ifindex(struct team_port *port)
{
	return port->ifindex;
}

/**
 * team_get_port_speed:
 * @port: port structure
 *
 * Get port speed.
 *
 * Returns: port speed in Mbits/s.
 **/
TEAM_EXPORT
uint32_t team_get_port_speed(struct team_port *port)
{
	return port->speed;
}

/**
 * team_get_port_duplex:
 * @port: port structure
 *
 * Get port duplex.
 *
 * Returns: 0 = half-duplex, 1 = full-duplex
 **/
TEAM_EXPORT
uint8_t team_get_port_duplex(struct team_port *port)
{
	return port->duplex;
}

/**
 * team_is_port_link_up:
 * @port: port structure
 *
 * See if port link is up.
 *
 * Returns: true if port link is up.
 **/
TEAM_EXPORT
bool team_is_port_link_up(struct team_port *port)
{
	return port->linkup;
}

/**
 * team_is_port_changed:
 * @port: port structure
 *
 * See if port values got changed.
 *
 * Returns: true if port got changed.
 **/
TEAM_EXPORT
bool team_is_port_changed(struct team_port *port)
{
	return port->changed;
}

/**
 * SECTION: Options
 * @short_description: option getters/setters, option_list manipulators
 */

struct team_option {
	struct list_item	list;
	enum team_option_type	type;
	char *			name;
	void *			data;
	bool			changed;
};

static void flush_option_list(struct team_handle *th)
{
	struct team_option *option, *tmp;

	list_for_each_node_entry_safe(option, tmp, &th->option_list, list) {
		list_del(&option->list);
		free(option->name);
		free(option->data);
		free(option);
	}
}

static struct team_option *create_option(char *name, int opt_type,
					 void *data, int data_size,
					 bool changed)
{
	struct team_option *option;

	option = malloc(sizeof(struct team_option));
	if (!option)
		return NULL;

	option->name = malloc(sizeof(char) * (strlen(name) + 1));
	if (!option->name)
		goto err_alloc_name;

	option->data = malloc(data_size);
	if (!option->data)
		goto err_alloc_data;

	option->type = opt_type;
	option->changed = changed;
	strcpy(option->name, name);
	memcpy(option->data, data, data_size);

	return option;

err_alloc_data:
	free(option->name);

err_alloc_name:
	free(option);

	return NULL;
}

static struct team_option *__find_option(struct list_item *opt_head,
					 const char *name)
{
	struct team_option *option;

	list_for_each_node_entry(option, opt_head, list) {
		if (strcmp(option->name, name) == 0)
			return option;
	}
	return NULL;
}

static int get_options_handler(struct nl_msg *msg, void *arg)
{
	struct nlmsghdr *nlh = nlmsg_hdr(msg);
	struct team_handle *th = arg;
	struct nlattr *attrs[TEAM_ATTR_MAX + 1];
	struct nlattr *nl_option;
	struct nlattr *option_attrs[TEAM_ATTR_OPTION_MAX + 1];
	int i;
	uint32_t team_ifindex = 0;
	struct list_item tmp_list;

	list_init(&tmp_list);
	genlmsg_parse(nlh, 0, attrs, TEAM_ATTR_MAX, NULL);
	if (attrs[TEAM_ATTR_TEAM_IFINDEX])
		team_ifindex = nla_get_u32(attrs[TEAM_ATTR_TEAM_IFINDEX]);

	if (team_ifindex != th->ifindex)
		return NL_SKIP;

	if (!attrs[TEAM_ATTR_LIST_OPTION])
		return NL_SKIP;

	nla_for_each_nested(nl_option, attrs[TEAM_ATTR_LIST_OPTION], i) {
		struct team_option *option;
		char *name;
		bool changed;
		int nla_type;
		__u32 arg;
		int opt_type;
		void *data;
		int data_size;
		char *str;

		if (nla_parse_nested(option_attrs, TEAM_ATTR_OPTION_MAX,
				     nl_option, NULL)) {
			err(th, "Failed to parse nested attributes.");
			return NL_SKIP;
		}

		if (!option_attrs[TEAM_ATTR_OPTION_NAME] ||
		    !option_attrs[TEAM_ATTR_OPTION_TYPE] ||
		    !option_attrs[TEAM_ATTR_OPTION_DATA]) {
			return NL_SKIP;
		}
		name = nla_get_string(option_attrs[TEAM_ATTR_OPTION_NAME]);
		if (__find_option(&tmp_list, name)) {
			err(th, "Option named \"%s\" is already in list.", name);
			continue;
		}

		if (option_attrs[TEAM_ATTR_OPTION_CHANGED])
			changed = true;
		else
			changed = false;

		nla_type = nla_get_u32(option_attrs[TEAM_ATTR_OPTION_TYPE]);
		switch (nla_type) {
		case NLA_U32:
			arg = nla_get_u32(option_attrs[TEAM_ATTR_OPTION_DATA]);
			data = &arg;
			data_size = sizeof(__u32);
			opt_type = TEAM_OPTION_TYPE_U32;
			break;
		case NLA_STRING:
			str = nla_get_string(option_attrs[TEAM_ATTR_OPTION_DATA]);
			data = str;
			data_size = sizeof(char) * (strlen(str) + 1);
			opt_type = TEAM_OPTION_TYPE_STRING;
			break;
		default:
			err(th, "Unknown nla_type received.");
			continue;
		}

		option = create_option(name, opt_type, data, data_size, changed);
		list_add(&tmp_list, &option->list);
	}

	flush_option_list(th);
	list_move_nodes(&th->option_list, &tmp_list);

	set_call_change_handlers(th, TEAM_OPTION_CHANGE);
	return NL_SKIP;
}

static int get_options(struct team_handle *th)
{
	struct nl_msg *msg;
	int err;

	msg = nlmsg_alloc();
	if (!msg)
		return -ENOMEM;

	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, th->family, 0, 0,
			 TEAM_CMD_OPTIONS_GET, 0);
	NLA_PUT_U32(msg, TEAM_ATTR_TEAM_IFINDEX, th->ifindex);

	err = send_and_recv(th, msg, get_options_handler, th);
	if (err)
		return err;

	check_call_change_handlers(th, TEAM_OPTION_CHANGE);
	return 0;

nla_put_failure:
	nlmsg_free(msg);
	return -ENOBUFS;
}

/**
 * team_get_option_by_name:
 * @th: libteam library context
 * @name: option name
 *
 * Get option structure referred by option @name.
 *
 * Returns: pointer to option structure or NULL in case option is not found.
 **/
TEAM_EXPORT
struct team_option *team_get_option_by_name(struct team_handle *th,
					    const char *name)
{
	return __find_option(&th->option_list, name);
}

/**
 * team_get_next_option:
 * @th: libteam library context
 * @option: option structure
 *
 * Get next option in list.
 *
 * Returns: option next to @option passed.
 **/
TEAM_EXPORT
struct team_option *team_get_next_option(struct team_handle *th,
					 struct team_option *option)
{
	return list_get_next_node_entry(&th->option_list, option, list);
}

/**
 * team_get_option_name:
 * @option: option structure
 *
 * Get option name.
 *
 * Returns: pointer to string containing option name.
 **/
TEAM_EXPORT
char *team_get_option_name(struct team_option *option)
{
	return option->name;
}

/**
 * team_get_option_type:
 * @option: option structure
 *
 * Get option type.
 *
 * Returns: number identificating option type.
 **/
TEAM_EXPORT
enum team_option_type team_get_option_type(struct team_option *option)
{
	return option->type;
}

/**
 * team_get_option_value_u32:
 * @option: option structure
 *
 * Get option value as unsigned 32-bit number.
 *
 * Returns: number.
 **/
TEAM_EXPORT
uint32_t team_get_option_value_u32(struct team_option *option)
{
	return *((__u32 *) option->data);
}

/**
 * team_get_option_value_string:
 * @option: option structure
 *
 * Get option value as string.
 *
 * Returns: pointer to string.
 **/
TEAM_EXPORT
char *team_get_option_value_string(struct team_option *option)
{
	return option->data;
}

/**
 * team_is_option_changed:
 * @option: option structure
 *
 * See if option values got changed.
 *
 * Returns: true if option got changed.
 **/
TEAM_EXPORT
bool team_is_option_changed(struct team_option *option)
{
	return option->changed;
}

/**
 * team_get_option_value_by_name_u32:
 * @th: libteam library context
 * @name: option name
 * u32_ptr: where the value will be stored
 *
 * Get option referred by @name and store its value as unsigned 32-bit
 * number into @u32_ptr.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_get_option_value_by_name_u32(struct team_handle *th,
				      const char *name, uint32_t *u32_ptr)
{
	struct team_option *option;

	option = team_get_option_by_name(th, name);
	if (!option)
		return -ENOENT;
	*u32_ptr = team_get_option_value_u32(option);
	return 0;
}

/**
 * team_get_option_value_by_name_string:
 * @th: libteam library context
 * @name: option name
 * str_ptr: where the value will be stored
 *
 * Get option referred by @name and store its value as pointer to string
 * into @srt_ptr.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_get_option_value_by_name_string(struct team_handle *th,
					 const char *name, char **str_ptr)
{
	struct team_option *option;

	option = team_get_option_by_name(th, name);
	if (!option)
		return -ENOENT;
	*str_ptr = team_get_option_value_string(option);
	return 0;
}

static int set_option_value(struct team_handle *th, const char *opt_name,
			    const void *data, int opt_type)
{
	struct nl_msg *msg;
	struct nlattr *option_list;
	struct nlattr *option_item;
	int nla_type;

	switch (opt_type) {
	case TEAM_OPTION_TYPE_U32:
		nla_type = NLA_U32;
		break;
	case TEAM_OPTION_TYPE_STRING:
		nla_type = NLA_STRING;
		break;
	default:
		return -ENOENT;
	}

	msg = nlmsg_alloc();
	if (!msg)
		return -ENOMEM;

	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, th->family, 0, 0,
		    TEAM_CMD_OPTIONS_SET, 0);
	NLA_PUT_U32(msg, TEAM_ATTR_TEAM_IFINDEX, th->ifindex);
	option_list = nla_nest_start(msg, TEAM_ATTR_LIST_OPTION);
	if (!option_list)
		goto nla_put_failure;
	option_item = nla_nest_start(msg, TEAM_ATTR_ITEM_OPTION);
	if (!option_item)
		goto nla_put_failure;
	NLA_PUT_STRING(msg, TEAM_ATTR_OPTION_NAME, opt_name);
	NLA_PUT_U32(msg, TEAM_ATTR_OPTION_TYPE, nla_type);
	switch (nla_type) {
		case NLA_U32:
			NLA_PUT_U32(msg, TEAM_ATTR_OPTION_DATA, *((__u32 *) data));
			break;
		case NLA_STRING:
			NLA_PUT_STRING(msg, TEAM_ATTR_OPTION_DATA, (char *) data);
			break;
		default:
			goto nla_put_failure;
	}
	nla_nest_end(msg, option_item);
	nla_nest_end(msg, option_list);

	return send_and_recv(th, msg, NULL, NULL);

nla_put_failure:
	nlmsg_free(msg);
	return -ENOBUFS;
}

/**
 * team_set_option_value_by_name_u32:
 * @th: libteam library context
 * @name: option name
 * @val: value to be set
 *
 * Set 32-bit number type option.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_set_option_value_by_name_u32(struct team_handle *th,
				      const char *name, uint32_t val)
{
	return set_option_value(th, name, &val, TEAM_OPTION_TYPE_U32);
}

/**
 * team_set_option_value_by_name_string:
 * @th: libteam library context
 * @name: option name
 * @str: string to be set
 *
 * Set string type option.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_set_option_value_by_name_string(struct team_handle *th,
					 const char *name, const char *str)
{
	return set_option_value(th, name, str, TEAM_OPTION_TYPE_STRING);
}

/**
 * SECTION: Context functions
 * @short_description: Core context functions
 */

static int event_handler(struct nl_msg *msg, void *arg)
{
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

	switch (gnlh->cmd) {
	case TEAM_CMD_PORT_LIST_GET:
		return get_port_list_handler(msg, arg);
	case TEAM_CMD_OPTIONS_GET:
		return get_options_handler(msg, arg);
	}
	return NL_SKIP;
}

/**
 * team_alloc:
 *
 * Allocates library context, sockets, initializes rtnl netlink connection.
 *
 * Returns: new libteam library context
 **/
TEAM_EXPORT
struct team_handle *team_alloc(void)
{
	struct team_handle *th;
	const char *env;
	int err;

	th = malloc(sizeof(struct team_handle));
	if (!th)
		return NULL;

	memset(th, 0, sizeof(struct team_handle));

	th->log_fn = log_stderr;
	th->log_priority = LOG_ERR;
	/* environment overwrites config */
	env = getenv("TEAM_LOG");
	if (env != NULL)
		team_set_log_priority(th, log_priority(env));

	dbg(th, "team_handle %p created.", th);
	dbg(th, "log_priority=%d", th->log_priority);

	list_init(&th->port_list);
	list_init(&th->option_list);
	list_init(&th->change_handler.list);

	th->nl_sock = nl_socket_alloc();
	if (!th->nl_sock)
		goto err_sk_alloc;

	th->nl_sock_event = nl_socket_alloc();
	if (!th->nl_sock_event)
		goto err_sk_event_alloc;

	th->nl_cli.sock = nl_cli_alloc_socket();
	if (!th->nl_cli.sock)
		goto err_cli_sk_alloc;
	err = nl_cli_connect(th->nl_cli.sock, NETLINK_ROUTE);
	if (err)
		goto err_cli_connect;
	th->nl_cli.link_cache = nl_cli_link_alloc_cache(th->nl_cli.sock);
	if (!th->nl_cli.link_cache)
		goto err_cli_alloc_cache;

	return th;

err_cli_alloc_cache:
err_cli_connect:
	nl_socket_free(th->nl_cli.sock);

err_cli_sk_alloc:
	nl_socket_free(th->nl_sock_event);

err_sk_event_alloc:
	nl_socket_free(th->nl_sock);

err_sk_alloc:
	free(th);

	return NULL;
}

static int do_create(struct team_handle *th, const char *team_name, bool recreate)
{
	struct rtnl_link *link;
	int err;

	link = rtnl_link_alloc();
	if (!link)
		return -ENOMEM;

	if (team_name) {
		rtnl_link_set_name(link, team_name);

		if (recreate && team_ifname2ifindex(th, team_name)) {
			err = rtnl_link_delete(th->nl_cli.sock, link);
			if (err)
				goto errout;
		}
	}

	err = rtnl_link_set_type(link, "team");
	if (err)
		goto errout;

	err = rtnl_link_add(th->nl_cli.sock, link, NLM_F_CREATE | NLM_F_EXCL);

errout:
	rtnl_link_put(link);

	return -nl2syserr(err);
}

/**
 * team_create:
 * @th: libteam library context
 * @team_name: new team device name
 *
 * Create new team device by given name. If NULL is passed, name will be
 * allocated automatically.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_create(struct team_handle *th, const char *team_name)
{
	return do_create(th, team_name, false);
}

/**
 * team_recreate:
 * @th: libteam library context
 * @team_name: new team device name
 *
 * Does the same as team_create only if device with @team_name already
 * exists it will be deleted first.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_recreate(struct team_handle *th, const char *team_name)
{
	return do_create(th, team_name, true);
}

/**
 * team_destroy:
 * @th: libteam library context
 *
 * Destroys current initialized team device.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_destroy(struct team_handle *th)
{
	struct rtnl_link *link;
	int err;

	if (!th->ifindex)
		return -ENODEV;
	link = rtnl_link_alloc();
	if (!link)
		return -ENOMEM;
	rtnl_link_set_ifindex(link, th->ifindex);
	err = rtnl_link_delete(th->nl_cli.sock, link);
	rtnl_link_put(link);
	return -nl2syserr(err);
}

/**
 * team_init:
 * @th: libteam library context
 * @ifindex: team device interface index
 *
 * Do library context initialization. Sets up team generic netlink connection.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_init(struct team_handle *th, uint32_t ifindex)
{
	int err;
	int grp_id;

	if (!ifindex) {
		err(th, "Passed interface index %d is not valid.", ifindex);
		return -ENOENT;
	}
	th->ifindex = ifindex;

	nl_socket_disable_seq_check(th->nl_sock_event);

	err = genl_connect(th->nl_sock);
	if (err) {
		err(th, "Failed to connect to netlink sock.");
		return -nl2syserr(err);
	}

	err = genl_connect(th->nl_sock_event);
	if (err) {
		err(th, "Failed to connect to netlink event sock.");
		return -nl2syserr(err);
	}

	th->family = genl_ctrl_resolve(th->nl_sock, TEAM_GENL_NAME);
	if (th->family < 0) {
		err(th, "Failed to resolve netlink family.");
		return -nl2syserr(th->family);
	}

	grp_id = genl_ctrl_resolve_grp(th->nl_sock, TEAM_GENL_NAME,
				       TEAM_GENL_CHANGE_EVENT_MC_GRP_NAME);
	if (grp_id < 0) {
		err(th, "Failed to resolve netlink multicast groups.");
		return -nl2syserr(grp_id);
	}

	err = nl_socket_add_membership(th->nl_sock_event, grp_id);
	if (err < 0) {
		err(th, "Failed to add netlink membership.");
		return -nl2syserr(err);
	}

	nl_socket_modify_err_cb(th->nl_sock,NL_CB_CUSTOM,
				error_handler, &th->nl_sock_err);
	nl_socket_modify_cb(th->nl_sock, NL_CB_FINISH, NL_CB_CUSTOM,
			    finish_handler, &th->nl_sock_err);
	nl_socket_modify_cb(th->nl_sock, NL_CB_ACK, NL_CB_CUSTOM,
			    ack_handler, &th->nl_sock_err);
	nl_socket_modify_cb(th->nl_sock_event, NL_CB_VALID, NL_CB_CUSTOM,
			    event_handler, th);

	err = get_port_list(th);
	if (err) {
		err(th, "Failed to get port list.");
		return err;
	}

	err = get_options(th);
	if (err) {
		err(th, "Failed to get options.");
		return err;
	}

	return 0;
}

/**
 * team_free:
 * @th: libteam library context
 *
 * Do libraty context cleanup.
 *
 **/
TEAM_EXPORT
void team_free(struct team_handle *th)
{
	flush_port_list(th);
	flush_option_list(th);
	nl_cache_free(th->nl_cli.link_cache);
	nl_socket_free(th->nl_cli.sock);
	nl_socket_free(th->nl_sock_event);
	nl_socket_free(th->nl_sock);
	free(th);
}

/**
 * team_set_log_fn:
 * @th: libteam library context
 * @log_fn: function to be called for logging messages
 *
 * The built-in logging writes to stderr. It can be
 * overridden by a custom function, to plug log messages
 * into the user's logging functionality.
 *
 **/
TEAM_EXPORT
void team_set_log_fn(struct team_handle *th,
		     void (*log_fn)(struct team_handle *th, int priority,
				    const char *file, int line, const char *fn,
				    const char *format, va_list args))
{
	th->log_fn = log_fn;
	dbg(th, "Custom logging function %p registered.", log_fn);
}

/**
 * team_get_log_priority:
 * @th: libteam library context
 *
 * Returns: the current logging priority
 **/
TEAM_EXPORT
int team_get_log_priority(struct team_handle *th)
{
	return th->log_priority;
}

/**
 * team_set_log_priority:
 * @th: libteam library context
 * @priority: the new logging priority
 *
 * Set the current logging priority. The value controls which messages
 * are logged.
 **/
TEAM_EXPORT
void team_set_log_priority(struct team_handle *th, int priority)
{
	th->log_priority = priority;
}

/**
 * team_get_event_fd:
 * @th: libteam library context
 *
 * Get file descriptor of event socket. This allows library user
 * to put the fd to poll for example.
 *
 * Returns: socket file descriptor.
 **/
TEAM_EXPORT
int team_get_event_fd(struct team_handle *th)
{
	return nl_socket_get_fd(th->nl_sock_event);
}

/**
 * team_process_event:
 * @th: libteam library context
 *
 * Process event which happened on event socket. Beware this calls
 * nl_recvmsgs_default() which blocks so be sure to call this only
 * if there are some data to read on event socket file descriptor.
 *
 **/
TEAM_EXPORT
void team_process_event(struct team_handle *th)
{
	nl_recvmsgs_default(th->nl_sock_event);
	check_call_change_handlers(th, TEAM_ANY_CHANGE);
}

/**
 * team_check_events:
 * @th: libteam library context
 *
 * Check for events pending to be processed on event socket and process
 * them one by one. This is safe to be called even if no data present
 * on event socket file descriptor.
 *
 **/
TEAM_EXPORT
void team_check_events(struct team_handle *th)
{
	int err;
	fd_set rfds;
	int tfd = team_get_event_fd(th);
	int fdmax = tfd + 1;
	struct timeval tv;

	while (true) {
		tv.tv_sec = tv.tv_usec = 0;
		FD_ZERO(&rfds);
		FD_SET(tfd, &rfds);
		err = select(fdmax, &rfds, NULL, NULL, &tv);
		if (err == -1 && errno == EINTR)
			continue;
		if (err != -1 && FD_ISSET(tfd, &rfds))
			team_process_event(th);
		else
			break;
	}
}

/**
 * team_get_mode_name:
 * @th: libteam library context
 * @mode_name: where the mode name will be stored
 *
 * Get name of currect mode.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_get_mode_name(struct team_handle *th, char **mode_name)
{
	return team_get_option_value_by_name_string(th, "mode", mode_name);
}

/**
 * team_set_mode_name:
 * @th: libteam library context
 * @mode_name: name of mode to be set
 *
 * Set team mode.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_set_mode_name(struct team_handle *th, const char *mode_name)
{
	return team_set_option_value_by_name_string(th, "mode", mode_name);
}

/**
 * team_get_active_port:
 * @th: libteam library context
 * @ifindex: where the port interface index will be stored
 *
 * Get interface index of active port. Note this is possible only if
 * team is in "activebackup" mode.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_get_active_port(struct team_handle *th, uint32_t *ifindex)
{
	return team_get_option_value_by_name_u32(th, "activeport", ifindex);
}

/**
 * team_set_active_port:
 * @th: libteam library context
 * @ifindex: interface index of new active port
 *
 * Set new active port by give @ifindex. Note this is possible only if
 * team is in "activebackup" mode.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_set_active_port(struct team_handle *th, uint32_t ifindex)
{
	return team_set_option_value_by_name_u32(th, "activeport", ifindex);
}

/**
 * SECTION: RTNL helpers
 * @short_description: Route netlink helper function
 */

/**
 * team_ifname2ifindex:
 * @th: libteam library context
 * @ifname: interface name
 *
 * Looks up for interface of given name and gets its index.
 *
 * Returns: zero if interface is not found,
 *	    interface index as reffered by in kernel otherwise.
 **/
TEAM_EXPORT
uint32_t team_ifname2ifindex(struct team_handle *th, const char *ifname)
{
	if (cli_cache_refill(th))
		return 0;
	return rtnl_link_name2i(th->nl_cli.link_cache, ifname);
}

/**
 * team_ifindex2ifname:
 * @th: libteam library context
 * @ifindex: interface index
 * @ifname: where the interface name will be stored
 * @maxlen: length of ifname buffer
 *
 * Looks up for interface of given index and gets its name.
 *
 * Returns: NULL if interface is not found,
 *	    @ifname otherwise.
 **/
TEAM_EXPORT
char *team_ifindex2ifname(struct team_handle *th, uint32_t ifindex,
			  char *ifname, unsigned int maxlen)
{
	if (cli_cache_refill(th))
		return NULL;
	return rtnl_link_i2name(th->nl_cli.link_cache, ifindex, ifname, maxlen);
}

/**
 * team_port_add:
 * @th: libteam library context
 * @port_ifindex: port interface index
 *
 * Adds port into team.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_port_add(struct team_handle *th, uint32_t port_ifindex)
{
	int err;

	err = rtnl_link_enslave_ifindex(th->nl_cli.sock, th->ifindex,
					port_ifindex);
	return -nl2syserr(err);
}

/**
 * team_port_remove:
 * @th: libteam library context
 * @port_ifindex: port interface index
 *
 * Removes port from team.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_port_remove(struct team_handle *th, uint32_t port_ifindex)
{
	int err;

	err = rtnl_link_release_ifindex(th->nl_cli.sock, port_ifindex);
	return -nl2syserr(err);
}

/**
 * team_hwaddr_set:
 * @th: libteam library context
 * @ifindex: interface index
 * @addr: address to be set
 * @addr_len: length of addr
 *
 * Sets given hardware address (MAC) for network interface by given
 * interface index.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_hwaddr_set(struct team_handle *th, uint32_t ifindex,
		    const char *addr, unsigned int addr_len)
{
	struct rtnl_link *link;
	int err;
	struct nl_addr *nl_addr;

	link = rtnl_link_alloc();
	if (!link)
		return -ENOMEM;

	nl_addr = nl_addr_build(AF_UNSPEC, (void *) addr, addr_len);
	if (!nl_addr) {
		err = -ENOMEM;
		goto errout;
	}

	rtnl_link_set_ifindex(link, ifindex);
	rtnl_link_set_addr(link, nl_addr);

	err = rtnl_link_change(th->nl_cli.sock, link, link, 0);
	err = -nl2syserr(err);

	nl_addr_put(nl_addr);

errout:
	rtnl_link_put(link);
	return err;
}

/**
 * team_hwaddr_get:
 * @th: libteam library context
 * @ifindex: interface index
 * @addr: address will be written here
 * @addr_len: length of addr buffer
 *
 * Gets hardware address (MAC) of network interface by given
 * interface index.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_hwaddr_get(struct team_handle *th, uint32_t ifindex,
		    char *addr, unsigned int addr_len)
{
	struct rtnl_link *link;
	int err;
	struct nl_addr *nl_addr;

	err = rtnl_link_get_kernel(th->nl_cli.sock, ifindex, NULL, &link);
	if (err)
		return -nl2syserr(err);
	nl_addr = rtnl_link_get_addr(link);
	if (!nl_addr) {
		err = -ENOENT;
		goto errout;
	}

	if (nl_addr_get_len(nl_addr) != addr_len) {
		err = -EINVAL;
		goto errout;
	}

	memcpy(addr, nl_addr_get_binary_addr(nl_addr), addr_len);

errout:
	rtnl_link_put(link);
	return err;
}

/**
 * team_hwaddr_len_get:
 * @th: libteam library context
 * @ifindex: interface index
 *
 * Gets length of hardware address (MAC) of network interface by given
 * interface index.
 *
 * Returns: number of bytes on success or negative number in case of an error.
 **/
TEAM_EXPORT
int team_hwaddr_len_get(struct team_handle *th, uint32_t ifindex)
{
	struct rtnl_link *link;
	int err;
	struct nl_addr *nl_addr;

	err = rtnl_link_get_kernel(th->nl_cli.sock, ifindex, NULL, &link);
	if (err)
		return -nl2syserr(err);
	nl_addr = rtnl_link_get_addr(link);
	if (!nl_addr) {
		err = -ENOENT;
		goto errout;
	}

	err = nl_addr_get_len(nl_addr);

errout:
	rtnl_link_put(link);
	return err;
}
