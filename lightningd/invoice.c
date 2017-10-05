#include "invoice.h"
#include "jsonrpc.h"
#include "lightningd.h"
#include <ccan/str/hex/hex.h>
#include <ccan/structeq/structeq.h>
#include <ccan/tal/str/str.h>
#include <common/utils.h>
#include <inttypes.h>
#include <lightningd/log.h>
#include <sodium/randombytes.h>

struct invoice_waiter {
	struct list_node list;
	struct command *cmd;
};

struct invoices {
	/* Payments for r values we know about. */
	struct list_head invlist;
	/* Waiting for new invoices to be paid. */
	struct list_head invoice_waiters;
};

static struct invoice *find_inv(const struct list_head *list,
				const struct sha256 *rhash,
				enum invoice_status state)
{
	struct invoice *i;

	list_for_each(list, i, list) {
		if (structeq(rhash, &i->rhash) && i->state == state)
			return i;
	}
	return NULL;
}

struct invoice *find_unpaid(struct invoices *invs, const struct sha256 *rhash)
{
	return find_inv(&invs->invlist, rhash, UNPAID);
}

static struct invoice *find_paid(struct invoices *invs,
				 const struct sha256 *rhash)
{
	return find_inv(&invs->invlist, rhash, PAID);
}

static struct invoice *find_invoice_by_label(const struct list_head *list,
					     const char *label)
{
	struct invoice *i;

	list_for_each(list, i, list) {
		if (streq(i->label, label))
			return i;
	}
	return NULL;
}

void invoice_add(struct invoices *invs,
		 struct invoice *inv)
{
	tal_steal(invs, inv);
	struct invoice *invoice = tal(invs, struct invoice);
	sha256(&invoice->rhash, invoice->r.r, sizeof(invoice->r.r));
	list_add(&invs->invlist, &inv->list);
}

struct invoices *invoices_init(const tal_t *ctx)
{
	struct invoices *invs = tal(ctx, struct invoices);

	list_head_init(&invs->invlist);
	list_head_init(&invs->invoice_waiters);

	return invs;
}

static void tell_waiter(struct command *cmd, const struct invoice *paid)
{
	struct json_result *response = new_json_result(cmd);

	json_object_start(response, NULL);
	json_add_string(response, "label", paid->label);
	json_add_hex(response, "rhash", &paid->rhash, sizeof(paid->rhash));
	json_add_u64(response, "msatoshi", paid->msatoshi);
	json_add_bool(response, "complete", paid->state == PAID);
	json_object_end(response);
	command_success(cmd, response);
}

void resolve_invoice(struct lightningd *ld, struct invoice *invoice)
{
	struct invoice_waiter *w;
	struct invoices *invs = ld->invoices;

	invoice->state = PAID;

	/* Tell all the waiters about the new paid invoice */
	while ((w = list_pop(&invs->invoice_waiters,
			     struct invoice_waiter,
			     list)) != NULL)
		tell_waiter(w->cmd, invoice);

	wallet_invoice_save(ld->wallet, invoice);
}

static void json_invoice(struct command *cmd,
			 const char *buffer, const jsmntok_t *params)
{
	struct invoice *invoice;
	jsmntok_t *msatoshi, *r, *label;
	struct json_result *response = new_json_result(cmd);
	struct invoices *invs = cmd->ld->invoices;

	if (!json_get_params(buffer, params,
			     "amount", &msatoshi,
			     "label", &label,
			     "?r", &r,
			     NULL)) {
		command_fail(cmd, "Need {amount} and {label}");
		return;
	}

	invoice = tal(cmd, struct invoice);
	invoice->id = 0;
	invoice->state = UNPAID;
	if (r) {
		if (!hex_decode(buffer + r->start, r->end - r->start,
				invoice->r.r, sizeof(invoice->r.r))) {
			command_fail(cmd, "Invalid hex r '%.*s'",
				     r->end - r->start, buffer + r->start);
			return;
		}
	} else
		randombytes_buf(invoice->r.r, sizeof(invoice->r.r));

	sha256(&invoice->rhash, invoice->r.r, sizeof(invoice->r.r));
	if (find_unpaid(invs, &invoice->rhash)
	    || find_paid(invs, &invoice->rhash)) {
		command_fail(cmd, "Duplicate r value '%s'",
			     tal_hexstr(cmd, &invoice->rhash,
					sizeof(invoice->rhash)));
		return;
	}

	if (!json_tok_u64(buffer, msatoshi, &invoice->msatoshi)
	    || invoice->msatoshi == 0) {
		command_fail(cmd, "'%.*s' is not a valid positive number",
			     msatoshi->end - msatoshi->start,
			     buffer + msatoshi->start);
		return;
	}

	invoice->label = tal_strndup(invoice, buffer + label->start,
				     label->end - label->start);
	if (find_invoice_by_label(&invs->invlist, invoice->label)) {
		command_fail(cmd, "Duplicate label '%s'", invoice->label);
		return;
	}
	if (strlen(invoice->label) > INVOICE_MAX_LABEL_LEN) {
		command_fail(cmd, "label '%s' over %u bytes", invoice->label,
			     INVOICE_MAX_LABEL_LEN);
		return;
	}

	if (!wallet_invoice_save(cmd->ld->wallet, invoice)) {
		printf("Could not save the invoice to the database: %s",
			  cmd->ld->wallet->db->err);
		command_fail(cmd, "database error");
		return;
	}

	/* OK, connect it to main state, respond with hash */
	tal_steal(invs, invoice);
	list_add(&invs->invlist, &invoice->list);

	json_object_start(response, NULL);
	json_add_hex(response, "rhash",
		     &invoice->rhash, sizeof(invoice->rhash));
	json_object_end(response);

	command_success(cmd, response);
}

static const struct json_command invoice_command = {
	"invoice",
	json_invoice,
	"Create invoice for {msatoshi} with {label} (with a set {r}, otherwise generate one)",
	"Returns the {rhash} on success. "
};
AUTODATA(json_command, &invoice_command);

static void json_add_invoices(struct json_result *response,
			      const struct list_head *list,
			      const char *buffer, const jsmntok_t *label)
{
	struct invoice *i;
	char *lbl = NULL;
	if (label)
		lbl = tal_strndup(response, &buffer[label->start], label->end - label->start);

	list_for_each(list, i, list) {
		if (lbl && !streq(i->label, lbl))
			continue;
		json_object_start(response, NULL);
		json_add_string(response, "label", i->label);
		json_add_hex(response, "rhash", &i->rhash, sizeof(i->rhash));
		json_add_u64(response, "msatoshi", i->msatoshi);
		json_add_bool(response, "complete", i->state == PAID);
		json_object_end(response);
	}
}

static void json_listinvoice(struct command *cmd,
			     const char *buffer, const jsmntok_t *params)
{
	jsmntok_t *label = NULL;
	struct json_result *response = new_json_result(cmd);
	struct invoices *invs = cmd->ld->invoices;

	if (!json_get_params(buffer, params,
			     "?label", &label,
			     NULL)) {
		command_fail(cmd, "Invalid arguments");
		return;
	}


	json_array_start(response, NULL);
	json_add_invoices(response, &invs->invlist, buffer, label);
	json_array_end(response);
	command_success(cmd, response);
}

static const struct json_command listinvoice_command = {
	"listinvoice",
	json_listinvoice,
	"Show invoice {label} (or all, if no {label}))",
	"Returns an array of {label}, {rhash}, {msatoshi} and {complete} on success. "
};
AUTODATA(json_command, &listinvoice_command);

static void json_delinvoice(struct command *cmd,
			    const char *buffer, const jsmntok_t *params)
{
	struct invoice *i;
	jsmntok_t *labeltok;
	struct json_result *response = new_json_result(cmd);
	const char *label;
	struct invoices *invs = cmd->ld->invoices;

	if (!json_get_params(buffer, params,
			     "label", &labeltok,
			     NULL)) {
		command_fail(cmd, "Invalid arguments");
		return;
	}

	label = tal_strndup(cmd, buffer + labeltok->start,
			    labeltok->end - labeltok->start);
	i = find_invoice_by_label(&invs->invlist, label);
	if (!i) {
		command_fail(cmd, "Unknown invoice");
		return;
	}

	if (!wallet_invoice_remove(cmd->ld->wallet, i)) {
		log_broken(cmd->ld->log, "Error attempting to remove invoice %"PRIu64": %s",
			   i->id, cmd->ld->wallet->db->err);
		command_fail(cmd, "Database error");
		return;
	}
	list_del_from(&invs->invlist, &i->list);

	json_object_start(response, NULL);
	json_add_string(response, "label", i->label);
	json_add_hex(response, "rhash", &i->rhash, sizeof(i->rhash));
	json_add_u64(response, "msatoshi", i->msatoshi);
	json_object_end(response);
	command_success(cmd, response);
	tal_free(i);
}

static const struct json_command delinvoice_command = {
	"delinvoice",
	json_delinvoice,
	"Delete unpaid invoice {label}))",
	"Returns {label}, {rhash} and {msatoshi} on success. "
};
AUTODATA(json_command, &delinvoice_command);

static void json_waitanyinvoice(struct command *cmd,
			    const char *buffer, const jsmntok_t *params)
{
	struct invoice *i;
	jsmntok_t *labeltok;
	const char *label = NULL;
	struct invoice_waiter *w;
	struct invoices *invs = cmd->ld->invoices;

	if (!json_get_params(buffer, params,
			     "?label", &labeltok,
			     NULL)) {
		command_fail(cmd, "Invalid arguments");
		return;
	}

	if (!labeltok) {
		i = list_top(&invs->invlist, struct invoice, list);

		/* Advance until we find a PAID one */
		while (i && i->state == UNPAID) {
			i = list_next(&invs->invlist, i, list);
		}
	} else {
		label = tal_strndup(cmd, buffer + labeltok->start,
				    labeltok->end - labeltok->start);
		i = find_invoice_by_label(&invs->invlist, label);
		if (!i) {
			command_fail(cmd, "Label not found");
			return;
		}
		while (i && i->state == UNPAID) {
			i = list_next(&invs->invlist, i, list);
		}
	}

	/* If we found one, return it. */
	if (i) {
		tell_waiter(cmd, i);
		return;
	}

	/* Otherwise, wait. */
	/* FIXME: Better to use io_wait directly? */
	w = tal(cmd, struct invoice_waiter);
	w->cmd = cmd;
	list_add_tail(&invs->invoice_waiters, &w->list);
}

static const struct json_command waitanyinvoice_command = {
	"waitanyinvoice",
	json_waitanyinvoice,
	"Wait for the next invoice to be paid, after {label} (if supplied)))",
	"Returns {label}, {rhash} and {msatoshi} on success. "
};
AUTODATA(json_command, &waitanyinvoice_command);


/* Wait for an incoming payment matching the `label` in the JSON
 * command.  This will either return immediately if the payment has
 * already been received or it may add the `cmd` to the list of
 * waiters, if the payment is still pending.
 */
static void json_waitinvoice(struct command *cmd,
			      const char *buffer, const jsmntok_t *params)
{
	struct invoice *i;
	jsmntok_t *labeltok;
	const char *label = NULL;
	struct invoice_waiter *w;
	struct invoices *invs = cmd->ld->invoices;

	if (!json_get_params(buffer, params, "label", &labeltok, NULL)) {
		command_fail(cmd, "Missing {label}");
		return;
	}

	/* Search in paid invoices, if found return immediately */
	label = tal_strndup(cmd, buffer + labeltok->start, labeltok->end - labeltok->start);
	i = find_invoice_by_label(&invs->invlist, label);

	if (!i) {
		command_fail(cmd, "Label not found");
		return;
	} else if (i->state == PAID) {
		tell_waiter(cmd, i);
		return;
	} else {
		/* There is an unpaid one matching, let's wait... */
		w = tal(cmd, struct invoice_waiter);
		w->cmd = cmd;
		list_add_tail(&invs->invoice_waiters, &w->list);
	}
}

static const struct json_command waitinvoice_command = {
	"waitinvoice",
	json_waitinvoice,
	"Wait for an incoming payment matching the invoice with {label}",
	"Returns {label}, {rhash} and {msatoshi} on success"
};
AUTODATA(json_command, &waitinvoice_command);
