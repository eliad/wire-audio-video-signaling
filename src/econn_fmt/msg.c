/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <re.h>
#include "avs_log.h"
#include "avs_jzon.h"
#include "avs_uuid.h"
#include "avs_zapi.h"
#include "avs_media.h"
#include "avs_econn.h"
#include "avs_econn_fmt.h"


const char econn_proto_version[] = "3.0";


static int econn_props_encode(struct json_object *jobj,
			      const struct econn_props *props)
{
	struct odict *odict_target;
	int err = 0;

	if (!jobj || !props)
		return EINVAL;

	odict_target = jzon_get_odict(jobj);

	err = odict_entry_add(odict_target, "props",
			      ODICT_OBJECT, props->dict);
	if (err)
		return err;

	return err;
}


int econn_message_encode(char **strp, const struct econn_message *msg)
{
	struct json_object *jobj = NULL;
	char *str = NULL;
	int err;

	if (!strp || !msg)
		return EINVAL;

	err = jzon_creatf(&jobj, "sss",
			  "version", econn_proto_version,
			  "type",   econn_msg_name(msg->msg_type),
			  "sessid", msg->sessid_sender);
	if (err)
		return err;

	err = jzon_add_bool(jobj, "resp", msg->resp);
	if (err)
		goto out;

	switch (msg->msg_type) {

	case ECONN_SETUP:
	case ECONN_UPDATE:
		err = jzon_add_str(jobj, "sdp", msg->u.setup.sdp_msg);
		if (err)
			goto out;

		/* props is optional for SETUP */
		if (msg->u.setup.props) {
			err = econn_props_encode(jobj, msg->u.setup.props);
			if (err)
				goto out;
		}
		break;

	case ECONN_CANCEL:
		break;

	case ECONN_HANGUP:
		break;

	case ECONN_PROPSYNC:

		/* props is mandatory for PROPSYNC */
		if (!msg->u.propsync.props) {
			warning("propsync: missing props\n");
			err = EINVAL;
			goto out;
		}

		err = econn_props_encode(jobj, msg->u.propsync.props);
		if (err)
			goto out;
		break;

	default:
		warning("econn: dont know how to encode %d\n", msg->msg_type);
		err = EBADMSG;
		break;
	}
	if (err)
		goto out;

	err = jzon_encode(&str, jobj);
	if (err)
		goto out;

 out:
	mem_deref(jobj);
	if (err)
		mem_deref(str);
	else
		*strp = str;

	return err;
}


static int econn_props_decode(struct econn_props **props,
			      struct json_object *jobj)
{
	struct json_object *jobj_props;
	int err;

	if (!props || !jobj)
		return EINVAL;

	/* get the "props" subtype */
	err = jzon_object(&jobj_props, jobj, "props");
	if (err) {
		warning("econn: no props\n");
		return err;
	}

	err = econn_props_alloc(props, jzon_get_odict(jobj_props));
	if (err) {
		warning("econn: econn_props_alloc error\n");
		return err;
	}

	return err;
}


int econn_message_decode(struct econn_message **msgp,
			 uint64_t curr_time, uint64_t msg_time,
			 const char *str, size_t len)
{
	struct econn_message *msg = NULL;
	struct json_object *jobj = NULL;
	const char *ver, *type, *sdp, *sessid;
	int err;

	if (!msgp || !str)
		return EINVAL;

	err = jzon_decode(&jobj, str, len);
	if (err)
		return err;

	msg = econn_message_alloc();
	if (!msg) {
		err = ENOMEM;
		goto out;
	}

	ver = jzon_str(jobj, "version");
	if (!ver) {
		warning("econn: missing 'version' field\n");
		err = EBADMSG;
		goto out;
	}

	if (0 != str_casecmp(econn_proto_version, ver)) {
		warning("econn: version mismatch (us=%s, msg=%s)\n",
			econn_proto_version, ver);
		err = EPROTO;
		goto out;
	}

	type = jzon_str(jobj, "type");
	if (!type) {
		warning("econn: missing 'type' field\n");
		err = EBADMSG;
		goto out;
	}

	sessid = jzon_str(jobj, "sessid");
	if (!sessid) {
		warning("econn: missing 'sessid' field\n");
		goto out;
	}
	str_ncpy(msg->sessid_sender, sessid, sizeof(msg->sessid_sender));

	err = jzon_bool(&msg->resp, jobj, "resp");
	if (err) {
		warning("econn: missing 'resp' field\n");
		goto out;
	}

	if (0 == str_casecmp(type, "setup")) {

		msg->msg_type = ECONN_SETUP;

		sdp = jzon_str(jobj, "sdp");
		if (!sdp) {
			warning("econn: missing 'sdp' field\n");
			err = EBADMSG;
			goto out;
		}

		err = str_dup(&msg->u.setup.sdp_msg, sdp);
		if (err)
			goto out;

		err = econn_props_decode(&msg->u.setup.props, jobj);
		if (err)
			goto out;
	}
	else if (0 == str_casecmp(type, "update")) {

		msg->msg_type = ECONN_UPDATE;

		sdp = jzon_str(jobj, "sdp");
		if (!sdp) {
			warning("econn: missing 'sdp' field\n");
			err = EBADMSG;
			goto out;
		}

		err = str_dup(&msg->u.setup.sdp_msg, sdp);
		if (err)
			goto out;

		err = econn_props_decode(&msg->u.setup.props, jobj);
		if (err)
			info("econn: decode UPDATE: no props\n");
	}	
	else if (0 == str_casecmp(type, "cancel")) {

		msg->msg_type = ECONN_CANCEL;
	}
	else if (0 == str_casecmp(type, "hangup")) {

		msg->msg_type = ECONN_HANGUP;
	}
	else if (0 == str_casecmp(type, "propsync")) {

		msg->msg_type = ECONN_PROPSYNC;

		err = econn_props_decode(&msg->u.propsync.props, jobj);
		if (err)
			goto out;
	}
	else {
		warning("econn: decode: unknown message type '%s'\n", type);
		err = EBADMSG;
		goto out;
	}

	msg->time = msg_time;
	msg->age = (msg_time > curr_time) ? 0 : curr_time - msg_time;

#if 0
	re_printf("%H\n", jzon_encode_odict_pretty, jzon_get_odict(jobj));
#endif

 out:
	mem_deref(jobj);
	if (err)
		mem_deref(msg);
	else
		*msgp = msg;

	return err;
}
