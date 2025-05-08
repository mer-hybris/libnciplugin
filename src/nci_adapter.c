/*
 * Copyright (C) 2019-2025 Slava Monich <slava@monich.com>
 * Copyright (C) 2019-2021 Jolla Ltd.
 * Copyright (C) 2020 Open Mobile Platform LLC.
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer
 *     in the documentation and/or other materials provided with the
 *     distribution.
 *
 *  3. Neither the names of the copyright holders nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * any official policies, either expressed or implied.
 */

#include "nci_adapter_impl.h"
#include "nci_plugin_p.h"
#include "nci_plugin_log.h"

#include <nfc_adapter_impl.h>
#include <nfc_initiator_impl.h>
#include <nfc_target_impl.h>
#include <nfc_tag_t2.h>
#include <nfc_tag_t4.h>
#include <nfc_peer.h>

#include <nci_core.h>
#include <nci_util.h>

#include <gutil_misc.h>
#include <gutil_macros.h>

GLOG_MODULE_DEFINE("nciplugin");

/* NCI core events */
enum {
    CORE_EVENT_CURRENT_STATE,
    CORE_EVENT_NEXT_STATE,
    CORE_EVENT_INTF_ACTIVATED,
    CORE_EVENT_PARAMS_CHANGED,
    CORE_EVENT_COUNT
};

typedef struct nci_adapter_intf_info {
    NCI_RF_INTERFACE rf_intf;
    NCI_PROTOCOL protocol;
    NCI_MODE mode;
    GUtilData mode_param;
    GUtilData activation_param;
    NciModeParam* mode_param_parsed;
} NciAdapterIntfInfo;

/*==========================================================================*
 * NCI adapter state machine
 *
 *              Poll side                         Listen side
 *              ---------                         -----------
 *
 *                              +------+
 *        /---------+---------> | IDLE | <------------------------------\
 *        |         |           +------+                     card       |
 *        |         |            |    ^                    emulation ---|--v
 *        |         |            |    |                    (ISO-DEP)    |  |
 *        |         |            |    |     Does the          /         |  |
 *        |         |            | Unknown  interface ---- yes          |  |
 *        |   Deactivation       |  object  info match?       \         |  |
 *        |         |            v    |    /       |        Anything    |  |
 *        |         |      Activation |   no    Activation    else      |  |
 *        |         |        ^    \   /  /         ^           |        |  |
 *        |         |       /      \ /  /          |           v        |  |
 *        |  +-------------+      Object        +----------------+      |  |
 *        |  | HAVE_TARGET | <-- detection ---> | HAVE_INITIATOR |      |  |
 *        |  +-------------+        ^           +----------------+      |  |
 *        |         |     ^         |                   |               |  |
 *        |         |      \        |                   v               |  |
 *        |         |       \       |              Deactivation         |  |
 *        |  nfcd-initiated  |      |                /      \           |  |
 *        |   reactivation   |      |               /        \          |  |
 *        |         |        |      |             Card       Anything --+  |
 *        |         |        |      |           emulation      else    /   |
 *  nfcd-initiated  |        |      |           (ISO-DEP)             /    |
 *   deactivation   |        |      |               |          Timeout     |
 *        ^         |        |      |               |             ^        |
 *        |         v        |      |               v             |        |
 *  +---------------------+  |      |            +-----------------+       |
 *  | REACTIVATING_TARGET |  |      |            | REACTIVATING_CE |       |
 *  +---------------------+  ^      |            +-----------------+       |
 *             |            /       |              |              ^        |
 *             v           /        ^              v              |        |
 *        Activation      /        / \        Activation          |        |
 *             |         /        /   no        /                 |        |
 *             |       yes       /      \      /             Deactivation  |
 *           Does the  /        /       Does the                  |        |
 *           interface ------- no       interface --- Activation  |        |
 *           info match?                info match?       ^       |        |
 *                                             |          |       |        |
 *                                             |     +----------------+    |
 *                                            yes--->| REACTIVATED_CE |<---/
 *                                                   +----------------+
 *
 *==========================================================================*/

typedef enum nci_adapter_state {
    NCI_ADAPTER_IDLE,
    NCI_ADAPTER_HAVE_TARGET,
    NCI_ADAPTER_HAVE_INITIATOR,
    NCI_ADAPTER_REACTIVATING_TARGET,
    NCI_ADAPTER_REACTIVATING_CE,
    NCI_ADAPTER_REACTIVATED_CE
} NCI_ADAPTER_STATE;

struct nci_adapter_priv {
    gulong nci_event_id[CORE_EVENT_COUNT];
    NFC_MODE desired_mode;
    NFC_MODE current_mode;
    gboolean mode_change_pending;
    guint mode_check_id;
    guint presence_check_id;
    guint presence_check_timer;
    NciAdapterIntfInfo* active_intf;
    NfcInitiator* initiator;
    NCI_ADAPTER_STATE internal_state;
    guint ce_reactivation_timer;
    NFC_ADAPTER_PARAM* supported_params;
    NCI_TECH supported_techs;
    NCI_TECH active_techs;
    NCI_TECH active_tech_mask;
    NfcTag* tag;     /* Weak pointer */
    NfcHost* host;   /* Weak pointer */
    NfcPeer* peer;   /* Weak pointer */
};

#define THIS(obj) NCI_ADAPTER(obj)
#define THIS_TYPE NCI_TYPE_ADAPTER
#define PARENT_TYPE NFC_TYPE_ADAPTER
#define PARENT_CLASS nci_adapter_parent_class
#define GET_THIS_CLASS(obj) G_TYPE_INSTANCE_GET_CLASS(obj, THIS_TYPE, \
        NciAdapterClass)

G_DEFINE_ABSTRACT_TYPE(NciAdapter, nci_adapter, PARENT_TYPE)

#define PRESENCE_CHECK_PERIOD_MS (250)
#define CE_REACTIVATION_TIMEOUT_MS (1500)

#define RANDOM_UID_SIZE (4)
#define RANDOM_UID_START_BYTE (0x08)

/*==========================================================================*
 * Implementation
 *==========================================================================*/

#if GUTIL_LOG_DEBUG
static
const char*
nci_adapter_internal_state_name(
    NCI_ADAPTER_STATE state)
{
    switch (state) {
    #define NCI_ADAPTER_(x) case NCI_ADAPTER_##x: return #x
    NCI_ADAPTER_(IDLE);
    NCI_ADAPTER_(HAVE_TARGET);
    NCI_ADAPTER_(HAVE_INITIATOR);
    NCI_ADAPTER_(REACTIVATING_TARGET);
    NCI_ADAPTER_(REACTIVATING_CE);
    NCI_ADAPTER_(REACTIVATED_CE);
    #undef NCI_ADAPTER_
    }
    return "?";
}
#endif

static
void
nci_adapter_set_internal_state(
    NciAdapterPriv* priv,
    NCI_ADAPTER_STATE state)
{
    if (priv->internal_state != state) {
        GDEBUG("Internal state %s => %s",
            nci_adapter_internal_state_name(priv->internal_state),
            nci_adapter_internal_state_name(state));
        priv->internal_state = state;
    }
}

static
NciAdapterIntfInfo*
nci_adapter_intf_info_new(
    const NciIntfActivationNtf* ntf)
{
    if (ntf) {
        /* Allocate the whole thing from a single memory block */
        const gsize total = G_ALIGN8(sizeof(NciAdapterIntfInfo)) +
            G_ALIGN8(ntf->mode_param_len) + ntf->activation_param_len;
        NciAdapterIntfInfo* info = g_malloc(total);
        guint8* ptr = (guint8*)info;

        info->rf_intf = ntf->rf_intf;
        info->protocol = ntf->protocol;
        info->mode = ntf->mode;
        ptr += G_ALIGN8(sizeof(NciAdapterIntfInfo));

        info->mode_param.size = ntf->mode_param_len;
        if (ntf->mode_param_len) {
            info->mode_param.bytes = ptr;
            memcpy(ptr, ntf->mode_param_bytes, ntf->mode_param_len);
            ptr += G_ALIGN8(ntf->mode_param_len);
        } else {
            info->mode_param.bytes = NULL;
        }

        info->activation_param.size = ntf->activation_param_len;
        if (ntf->activation_param_len) {
            info->activation_param.bytes = ptr;
            memcpy(ptr, ntf->activation_param_bytes, ntf->activation_param_len);
        } else {
            info->activation_param.bytes = NULL;
        }

        info->mode_param_parsed = nci_util_copy_mode_param(ntf->mode_param,
            ntf->mode);

        return info;
    }
    return NULL;
}

static
void
nci_adapter_intf_info_free(
    NciAdapterIntfInfo* info)
{
    if (info) {
        g_free(info->mode_param_parsed);
        g_free(info);
    }
}

static
gboolean
mode_param_match_poll_a(
    const NciModeParamPollA* pa1,
    const NciModeParamPollA* pa2)
{
    if (pa1->sel_res == pa2->sel_res &&
        pa1->sel_res_len == pa2->sel_res_len &&
        pa1->nfcid1_len == pa2->nfcid1_len &&
        !memcmp(pa1->sens_res, pa2->sens_res, sizeof(pa2->sens_res))) {

        /*
         * As specified in NFCForum-TS-DigitalProtocol-1.0, in case of
         * a single size NFCID1 (4 Bytes), a value of nfcid10 set to 08h
         * indicates that nfcid11 to nfcid13 SHALL be dynamically generated.
         */
        if (pa1->nfcid1_len == RANDOM_UID_SIZE &&
            pa1->nfcid1[0] == pa2->nfcid1[0] &&
            pa2->nfcid1[0] == RANDOM_UID_START_BYTE) {
            return TRUE;
        } else {
            /* Otherwise UID should fully match */
            return !memcmp(pa1->nfcid1, pa2->nfcid1, pa2->nfcid1_len);
        }
    }
    return FALSE;
}

static
gboolean
mode_param_match_poll_b(
    const NciModeParamPollB* pb1,
    const NciModeParamPollB* pb2)
{
    /*
    * Compare all fields except UID 'cause UID may be
    * changed after losing field
    */
    return pb1->fsc == pb2->fsc &&
        !memcmp(pb1->app_data, pb2->app_data, sizeof(pb2->app_data)) &&
        pb1->prot_info.size == pb2->prot_info.size &&
        gutil_data_equal(&pb1->prot_info, &pb2->prot_info);
}

static
gboolean
nci_adapter_info_mode_params_matches(
    const NciAdapterIntfInfo* info,
    const NciIntfActivationNtf* ntf)
{
    const NciModeParam* mp1 = info->mode_param_parsed;
    const NciModeParam* mp2 = ntf->mode_param;

    if (mp1 && mp2) {
        /* Mode params criteria depends on type of tag */
        switch (ntf->mode) {
        case NCI_MODE_PASSIVE_POLL_A:
            switch (ntf->rf_intf) {
            case NCI_RF_INTERFACE_FRAME:    /* Type 2 Tag */
            case NCI_RF_INTERFACE_ISO_DEP:  /* ISO-DEP Type 4A */
                return mode_param_match_poll_a(&mp1->poll_a, &mp2->poll_a);
            case NCI_RF_INTERFACE_NFCEE_DIRECT:
            case NCI_RF_INTERFACE_NFC_DEP:
            case NCI_RF_INTERFACE_PROPRIETARY:
                break;
            }
            break;
        case NCI_MODE_PASSIVE_POLL_B:
            switch (ntf->rf_intf) {
            case NCI_RF_INTERFACE_ISO_DEP:
                /* ISO-DEP Type 4B */
                return mode_param_match_poll_b(&mp1->poll_b, &mp2->poll_b);
            case NCI_RF_INTERFACE_FRAME:
            case NCI_RF_INTERFACE_NFCEE_DIRECT:
            case NCI_RF_INTERFACE_NFC_DEP:
            case NCI_RF_INTERFACE_PROPRIETARY:
                break;
            }
            break;
        case NCI_MODE_ACTIVE_POLL_A:
        case NCI_MODE_PASSIVE_POLL_F:
        case NCI_MODE_ACTIVE_POLL_F:
        case NCI_MODE_PASSIVE_POLL_15693:
        case NCI_MODE_PASSIVE_LISTEN_A:
        case NCI_MODE_PASSIVE_LISTEN_B:
        case NCI_MODE_PASSIVE_LISTEN_F:
        case NCI_MODE_ACTIVE_LISTEN_A:
        case NCI_MODE_ACTIVE_LISTEN_F:
        case NCI_MODE_PASSIVE_LISTEN_15693:
            break;
        }
    }
    /* Full match is expected in other cases */
    return info->mode_param.size == ntf->mode_param_len &&
        (!ntf->mode_param_len || !memcmp(info->mode_param.bytes,
            ntf->mode_param_bytes, ntf->mode_param_len));
}

static
gboolean
nci_adapter_intf_info_matches(
    const NciAdapterIntfInfo* info,
    const NciIntfActivationNtf* ntf)
{
    return info &&
        info->rf_intf == ntf->rf_intf &&
        info->protocol == ntf->protocol &&
        info->mode == ntf->mode &&
        nci_adapter_info_mode_params_matches(info, ntf) &&
        info->activation_param.size == ntf->activation_param_len &&
        (!ntf->activation_param_len || !memcmp(info->activation_param.bytes,
        ntf->activation_param_bytes, ntf->activation_param_len));
}

static
gpointer
nci_adapter_set_active_object(
    gpointer obj,
    gpointer* weak_ptr)
{
    if (*weak_ptr != obj) {
        if (*weak_ptr) {
            g_object_remove_weak_pointer(*weak_ptr, weak_ptr);
        }
        *weak_ptr = obj;
        if (obj) {
            g_object_add_weak_pointer(obj, weak_ptr);
        }
    }
    return obj;
}

static
NfcTag*
nci_adapter_set_active_tag(
    NciAdapterPriv* priv,
    NfcTag* tag)
{
    return nci_adapter_set_active_object(tag, (gpointer*) &priv->tag);
}

static
NfcPeer*
nci_adapter_set_active_peer(
    NciAdapterPriv* priv,
    NfcPeer* peer)
{
    return nci_adapter_set_active_object(peer, (gpointer*) &priv->peer);
}

static
NfcHost*
nci_adapter_set_active_host(
    NciAdapterPriv* priv,
    NfcHost* host)
{
    return nci_adapter_set_active_object(host, (gpointer*) &priv->host);
}

static
void
nci_adapter_clear_active_intf(
    NciAdapterPriv* priv)
{
    if (priv->active_intf) {
        nci_adapter_intf_info_free(priv->active_intf);
        priv->active_intf = NULL;
    }
}

static
void
nci_adapter_drop_target(
    NciAdapter* self)
{
    NfcTarget* target = self->target;

    if (target) {
        NciAdapterPriv* priv = self->priv;

        self->target = NULL;
        nci_adapter_clear_active_intf(priv);
        gutil_source_clear(&priv->presence_check_timer);
        nci_adapter_set_active_peer(priv, NULL);
        nci_adapter_set_active_tag(priv, NULL);
        if (priv->presence_check_id) {
            nfc_target_cancel_transmit(target, priv->presence_check_id);
            priv->presence_check_id = 0;
        }
        GINFO("Target is gone");
        nfc_target_gone(target);
        nfc_target_unref(target);
    }
}

static
void
nci_adapter_drop_initiator(
    NciAdapter* self)
{
    NciAdapterPriv* priv = self->priv;
    NfcInitiator* initiator = priv->initiator;

    if (initiator) {
        priv->initiator = NULL;
        priv->active_tech_mask = NCI_TECH_ALL;
        nci_adapter_clear_active_intf(priv);
        gutil_source_clear(&priv->ce_reactivation_timer);
        nci_adapter_set_active_peer(priv, NULL);
        nci_adapter_set_active_host(priv, NULL);
        nci_core_set_tech(self->nci, priv->active_techs);
        GINFO("Initiator is gone");
        nfc_initiator_gone(initiator);
        nfc_initiator_unref(initiator);
    }
}

static
void
nci_adapter_drop_all(
    NciAdapter* self)
{
    nci_adapter_drop_target(self);
    nci_adapter_drop_initiator(self);
}

static
gboolean
nci_adapter_need_presence_checks(
    NciAdapter* self)
{
    const NciAdapterIntfInfo* intf = self->priv->active_intf;

    /* NFC-DEP presence checks are done at LLCP level by NFC core */
    return (self->target && intf && intf->protocol != NCI_PROTOCOL_NFC_DEP);
}

static
void
nci_adapter_presence_check_done(
    NfcTarget* target,
    gboolean ok,
    void* user_data)
{
    NciAdapter* self = THIS(user_data);
    NciAdapterPriv* priv = self->priv;

    GDEBUG("Presence check %s", ok ? "ok" : "failed");
    priv->presence_check_id = 0;
    if (!ok) {
        nci_adapter_deactivate_target(self, target);
    }
}

static
gboolean
nci_adapter_presence_check_timer(
    gpointer user_data)
{
    NciAdapter* self = THIS(user_data);
    NciAdapterPriv* priv = self->priv;
    NfcTargetSequence* seq = self->target->sequence;
    gboolean do_presence_check = !seq || (nfc_target_sequence_flags(seq)
        & NFC_SEQUENCE_FLAG_ALLOW_PRESENCE_CHECK);

    if (!priv->presence_check_id && do_presence_check) {
        priv->presence_check_id = nci_target_presence_check(self->target,
            nci_adapter_presence_check_done, self);
        if (!priv->presence_check_id) {
            GDEBUG("Failed to start presence check");
            priv->presence_check_timer = 0;
            nci_core_set_state(self->nci, NCI_RFST_DISCOVERY);
            return G_SOURCE_REMOVE;
        }
    } else {
        GDEBUG("Skipped presence check");
    }
    return G_SOURCE_CONTINUE;
}

static
void
nci_adapter_mode_check(
    NciAdapter* self)
{
    NciCore* nci = self->nci;
    NciAdapterPriv* priv = self->priv;
    const NFC_MODE mode = (nci->current_state > NCI_RFST_IDLE) ?
        priv->desired_mode : NFC_MODE_NONE;

    gutil_source_clear(&priv->mode_check_id);
    if (priv->mode_change_pending) {
        if (mode == priv->desired_mode) {
            priv->mode_change_pending = FALSE;
            priv->current_mode = mode;
            nfc_adapter_mode_notify(NFC_ADAPTER(self), mode, TRUE);
        }
    } else if (priv->current_mode != mode) {
        priv->current_mode = mode;
        nfc_adapter_mode_notify(NFC_ADAPTER(self), mode, FALSE);
    }
}

static
gboolean
nci_adapter_mode_check_cb(
    gpointer user_data)
{
    NciAdapter* self = THIS(user_data);
    NciAdapterPriv* priv = self->priv;

    priv->mode_check_id = 0;
    nci_adapter_mode_check(self);
    return G_SOURCE_REMOVE;
}

static
void
nci_adapter_schedule_mode_check(
    NciAdapter* self)
{
    NciAdapterPriv* priv = self->priv;

    if (!priv->mode_check_id) {
        priv->mode_check_id = g_idle_add(nci_adapter_mode_check_cb, self);
    }
}

static
void
nci_adapter_state_check(
    NciAdapter* self)
{
    NciCore* nci = self->nci;

    if (nci->current_state == NCI_RFST_IDLE &&
        nci->next_state == NCI_RFST_IDLE) {
        NfcAdapter* adapter = &self->parent;

        if (adapter->enabled && adapter->powered && adapter->power_requested) {
            /*
             * State machine may have switched to RFST_IDLE in the process of
             * changing the operation mode or active technologies. Kick it
             * back to RFST_DISCOVERY.
             */
            nci_core_set_state(self->nci, NCI_RFST_DISCOVERY);
        }
    }
}

static
const NfcParamPollA*
nci_adapter_convert_poll_a(
    NfcParamPollA* dest,
    const NciModeParam* mp)
{
    if (mp) {
        const NciModeParamPollA* src = &mp->poll_a;

        dest->sel_res = src->sel_res;
        dest->nfcid1.bytes = src->nfcid1;
        dest->nfcid1.size = src->nfcid1_len;
        return dest;
    } else {
        return NULL;
    }
}

static
const NfcParamPollB*
nci_adapter_convert_poll_b(
    NfcParamPollB* dest,
    const NciModeParam* mp)
{
    if (mp) {
        const NciModeParamPollB* src = &mp->poll_b;

        dest->fsc = src->fsc;
        dest->nfcid0.bytes = src->nfcid0;
        dest->nfcid0.size = sizeof(src->nfcid0);
        dest->prot_info = src->prot_info;
        memcpy(dest->app_data, src->app_data, sizeof(src->app_data));
        return dest;
    } else {
        return NULL;
    }
}

static
const NfcParamPollF*
nci_adapter_convert_poll_f(
    NfcParamPollF* dest,
    const NciModeParam* mp)
{
    if (mp) {
        const NciModeParamPollF* src = &mp->poll_f;

        switch (src->bitrate) {
        case NFC_BIT_RATE_212:
            dest->bitrate = 212;
            break;
        case NFC_BIT_RATE_424:
            dest->bitrate = 424;
            break;
        default:
            /* The rest is RFU according to NCI 1.0 spec */
            dest->bitrate = 0;
            break;
        }
        dest->nfcid2.bytes = src->nfcid2;
        dest->nfcid2.size = sizeof(src->nfcid2);
        return dest;
    } else {
        return NULL;
    }
}

static
const NfcParamListenF*
nci_adapter_convert_listen_f(
    NfcParamListenF* dest,
    const NciModeParam* mp)
{
    if (mp) {
        const NciModeParamListenF* src = &mp->listen_f;

        dest->nfcid2 = src->nfcid2;
        return dest;
    } else {
        return NULL;
    }
}

static
const NfcParamIsoDepPollA*
nci_adapter_convert_iso_dep_poll_a(
    NfcParamIsoDepPollA* dest,
    const NciActivationParam* ap)
{
    if (ap) {
        const NciActivationParamIsoDepPollA* src = &ap->iso_dep_poll_a;

        dest->fsc = src->fsc;
        dest->t1 = src->t1;
        dest->t0 = src->t0;
        dest->ta = src->ta;
        dest->tb = src->tb;
        dest->tc = src->tc;
        return dest;
    } else {
        return NULL;
    }
}

static
const NfcParamIsoDepPollB*
nci_adapter_convert_iso_dep_poll_b(
    NfcParamIsoDepPollB* dest,
    const NciActivationParam* ap)
{
    if (ap) {
        const NciActivationParamIsoDepPollB* src = &ap->iso_dep_poll_b;

        dest->mbli = src->mbli;
        dest->did = src->did;
        dest->hlr = src->hlr;
        return dest;
    } else {
        return NULL;
    }
}

static
const NfcParamNfcDepInitiator*
nci_adapter_convert_nfc_dep_poll(
    NfcParamNfcDepInitiator* dest,
    const NciActivationParam* ap)
{
    if (ap) {
        const NciActivationParamNfcDepPoll* src = &ap->nfc_dep_poll;

        dest->atr_res_g = src->g;
        return dest;
    } else {
        return NULL;
    }
}

static
const NfcParamNfcDepTarget*
nci_adapter_convert_nfc_dep_listen(
    NfcParamNfcDepTarget* dest,
    const NciActivationParam* ap)
{
    if (ap) {
        const NciActivationParamNfcDepListen* src = &ap->nfc_dep_listen;

        dest->atr_req_g = src->g;
        return dest;
    } else {
        return NULL;
    }
}

static
NfcTag*
nci_adapter_create_known_tag(
    NciAdapter* self,
    NfcTarget* target,
    const NciIntfActivationNtf* ntf)
{
    const NciActivationParam* ap = ntf->activation_param;
    const NciModeParam* mp = ntf->mode_param;
    NfcParamIsoDepPollA iso_dep_poll_a;
    NfcParamIsoDepPollB iso_dep_poll_b;
    NfcParamPollA poll_a;
    NfcParamPollB poll_b;
    NfcTag* tag = NULL;

    /* Figure out what kind of target we are dealing with */
    switch (ntf->protocol) {
    case NCI_PROTOCOL_T2T:
        if (ntf->rf_intf == NCI_RF_INTERFACE_FRAME) {
            switch (ntf->mode) {
            case NCI_MODE_PASSIVE_POLL_A:
            case NCI_MODE_ACTIVE_POLL_A:
                /* Type 2 Tag */
                tag = nfc_adapter_add_tag_t2(NFC_ADAPTER(self), target,
                    nci_adapter_convert_poll_a(&poll_a, mp));
                break;
            case NCI_MODE_PASSIVE_POLL_B:
            case NCI_MODE_PASSIVE_POLL_F:
            case NCI_MODE_ACTIVE_POLL_F:
            case NCI_MODE_PASSIVE_POLL_15693:
            case NCI_MODE_PASSIVE_LISTEN_A:
            case NCI_MODE_PASSIVE_LISTEN_B:
            case NCI_MODE_PASSIVE_LISTEN_F:
            case NCI_MODE_ACTIVE_LISTEN_A:
            case NCI_MODE_ACTIVE_LISTEN_F:
            case NCI_MODE_PASSIVE_LISTEN_15693:
                break;
            }
        }
        break;
    case NCI_PROTOCOL_ISO_DEP:
        if (ntf->rf_intf == NCI_RF_INTERFACE_ISO_DEP) {
            switch (ntf->mode) {
            case NCI_MODE_PASSIVE_POLL_A:
                /* ISO-DEP Type 4A */
                tag = nfc_adapter_add_tag_t4a(NFC_ADAPTER(self), target,
                    nci_adapter_convert_poll_a(&poll_a, mp),
                    nci_adapter_convert_iso_dep_poll_a(&iso_dep_poll_a, ap));
                break;
            case NCI_MODE_PASSIVE_POLL_B:
                /* ISO-DEP Type 4B */
                tag = nfc_adapter_add_tag_t4b(NFC_ADAPTER(self), target,
                    nci_adapter_convert_poll_b(&poll_b, mp),
                    nci_adapter_convert_iso_dep_poll_b(&iso_dep_poll_b, ap));
                break;
            case NCI_MODE_ACTIVE_POLL_A:
            case NCI_MODE_PASSIVE_POLL_F:
            case NCI_MODE_ACTIVE_POLL_F:
            case NCI_MODE_PASSIVE_POLL_15693:
            case NCI_MODE_PASSIVE_LISTEN_A:
            case NCI_MODE_PASSIVE_LISTEN_B:
            case NCI_MODE_PASSIVE_LISTEN_F:
            case NCI_MODE_ACTIVE_LISTEN_A:
            case NCI_MODE_ACTIVE_LISTEN_F:
            case NCI_MODE_PASSIVE_LISTEN_15693:
                break;
            }
        }
        break;
    case NCI_PROTOCOL_T1T:
    case NCI_PROTOCOL_T3T:
    case NCI_PROTOCOL_T5T:
    case NCI_PROTOCOL_NFC_DEP:
    case NCI_PROTOCOL_PROPRIETARY:
    case NCI_PROTOCOL_UNDETERMINED:
        break;
    }
    return nci_adapter_set_active_tag(self->priv, tag);
}

static
NfcPeer*
nci_adapter_create_peer_initiator(
    NciAdapter* self,
    NfcTarget* target,
    const NciIntfActivationNtf* ntf)
{
    const NciActivationParam* ap = ntf->activation_param;
    const NciModeParam* mp = ntf->mode_param;
    NfcParamNfcDepInitiator nfc_dep;
    NfcParamPollA poll_a;
    NfcParamPollF poll_f;
    NfcPeer* peer = NULL;

    switch (ntf->protocol) {
    case NCI_PROTOCOL_NFC_DEP:
        if (ntf->rf_intf == NCI_RF_INTERFACE_NFC_DEP) {
            switch (ntf->mode) {
            case NCI_MODE_ACTIVE_POLL_A:
            case NCI_MODE_PASSIVE_POLL_A:
                /* NFC-DEP (Poll side) */
                peer = nfc_adapter_add_peer_initiator_a(NFC_ADAPTER(self),
                    target, nci_adapter_convert_poll_a(&poll_a, mp),
                    nci_adapter_convert_nfc_dep_poll(&nfc_dep, ap));
                break;
            case NCI_MODE_ACTIVE_POLL_F:
            case NCI_MODE_PASSIVE_POLL_F:
                /* NFC-DEP (Poll side) */
                peer = nfc_adapter_add_peer_initiator_f(NFC_ADAPTER(self),
                    target, nci_adapter_convert_poll_f(&poll_f, mp),
                    nci_adapter_convert_nfc_dep_poll(&nfc_dep, ap));
                break;
            case NCI_MODE_ACTIVE_LISTEN_A:
            case NCI_MODE_PASSIVE_LISTEN_A:
            case NCI_MODE_PASSIVE_POLL_B:
            case NCI_MODE_PASSIVE_POLL_15693:
            case NCI_MODE_PASSIVE_LISTEN_B:
            case NCI_MODE_PASSIVE_LISTEN_F:
            case NCI_MODE_ACTIVE_LISTEN_F:
            case NCI_MODE_PASSIVE_LISTEN_15693:
                break;
            }
        }
        break;
    case NCI_PROTOCOL_T1T:
    case NCI_PROTOCOL_T2T:
    case NCI_PROTOCOL_T3T:
    case NCI_PROTOCOL_T5T:
    case NCI_PROTOCOL_ISO_DEP:
    case NCI_PROTOCOL_PROPRIETARY:
    case NCI_PROTOCOL_UNDETERMINED:
        break;
    }
    return nci_adapter_set_active_peer(self->priv, peer);
}

static
NfcPeer*
nci_adapter_create_peer_target(
    NciAdapter* self,
    NfcInitiator* initiator,
    const NciIntfActivationNtf* ntf)
{
    const NciActivationParam* ap = ntf->activation_param;
    const NciModeParam* mp = ntf->mode_param;
    NfcParamNfcDepTarget nfc_dep;
    NfcParamListenF listen_f;
    NfcPeer* peer = NULL;

    switch (ntf->rf_intf) {
    case NCI_RF_INTERFACE_NFC_DEP:
        switch (ntf->mode) {
        case NCI_MODE_ACTIVE_LISTEN_A:
        case NCI_MODE_PASSIVE_LISTEN_A:
            /* NFC-DEP (Listen side) */
            peer = nfc_adapter_add_peer_target_a(NFC_ADAPTER(self), initiator,
                NULL, nci_adapter_convert_nfc_dep_listen(&nfc_dep, ap));
            break;
        case NCI_MODE_PASSIVE_LISTEN_F:
        case NCI_MODE_ACTIVE_LISTEN_F:
            /* NFC-DEP (Listen side) */
            peer = nfc_adapter_add_peer_target_f(NFC_ADAPTER(self), initiator,
                nci_adapter_convert_listen_f(&listen_f, mp),
                nci_adapter_convert_nfc_dep_listen(&nfc_dep, ap));
            break;
        case NCI_MODE_ACTIVE_POLL_A:
        case NCI_MODE_PASSIVE_POLL_A:
        case NCI_MODE_PASSIVE_POLL_B:
        case NCI_MODE_PASSIVE_POLL_F:
        case NCI_MODE_ACTIVE_POLL_F:
        case NCI_MODE_PASSIVE_POLL_15693:
        case NCI_MODE_PASSIVE_LISTEN_B:
        case NCI_MODE_PASSIVE_LISTEN_15693:
            break;
        }
        break;
    case NCI_RF_INTERFACE_FRAME:
    case NCI_RF_INTERFACE_ISO_DEP:
    case NCI_RF_INTERFACE_NFCEE_DIRECT:
    case NCI_RF_INTERFACE_PROPRIETARY:
        break;
    }
    return nci_adapter_set_active_peer(self->priv, peer);
}

static
NfcHost*
nci_adapter_create_host(
    NciAdapter* self,
    NfcInitiator* initiator,
    const NciIntfActivationNtf* ntf)
{
    NfcHost* host = NULL;

    switch (ntf->rf_intf) {
    case NCI_RF_INTERFACE_ISO_DEP:
        host = nfc_adapter_add_host(NFC_ADAPTER(self), initiator);
        break;
    case NCI_RF_INTERFACE_FRAME:
    case NCI_RF_INTERFACE_NFC_DEP:
    case NCI_RF_INTERFACE_NFCEE_DIRECT:
    case NCI_RF_INTERFACE_PROPRIETARY:
        break;
    }
    return nci_adapter_set_active_host(self->priv, host);
}

static
const NfcParamPoll*
nci_adapter_get_mode_param(
    NfcParamPoll* poll,
    const NciIntfActivationNtf* ntf)
{
    const NciModeParam* mp = ntf->mode_param;

    /* Figure out what kind of target we are dealing with */
    switch (ntf->mode) {
    case NCI_MODE_PASSIVE_POLL_A:
        if (nci_adapter_convert_poll_a(&poll->a, mp)) {
            return poll;
        }
        break;
    case NCI_MODE_PASSIVE_POLL_B:
        if (nci_adapter_convert_poll_b(&poll->b, mp)) {
            return poll;
        }
        break;
    case NCI_MODE_ACTIVE_POLL_A:
    case NCI_MODE_PASSIVE_POLL_F:
    case NCI_MODE_ACTIVE_POLL_F:
    case NCI_MODE_PASSIVE_POLL_15693:
    case NCI_MODE_PASSIVE_LISTEN_A:
    case NCI_MODE_PASSIVE_LISTEN_B:
    case NCI_MODE_PASSIVE_LISTEN_F:
    case NCI_MODE_ACTIVE_LISTEN_A:
    case NCI_MODE_ACTIVE_LISTEN_F:
    case NCI_MODE_PASSIVE_LISTEN_15693:
        break;
    }
    return NULL;
}

/*==========================================================================*
 * NCI adapter state machine events
 *==========================================================================*/

static
gboolean
nci_adapter_ce_reactivation_timeout(
    gpointer user_data)
{
    NciAdapter* self = THIS(user_data);
    NciAdapterPriv* priv = self->priv;

    GDEBUG("CE reactivation timeout has expired");
    priv->ce_reactivation_timer = 0;
    nci_adapter_set_internal_state(priv, NCI_ADAPTER_IDLE);
    nci_adapter_drop_all(self);
    return G_SOURCE_REMOVE;
}

static
void
nci_adapter_start_ce_reactivation_timer(
    NciAdapter* self)
{
    NciAdapterPriv* priv = self->priv;

    GDEBUG("%s CE reactivation timer", priv->ce_reactivation_timer ?
        "Restarting" : "Starting");
    gutil_source_remove(priv->ce_reactivation_timer);
    priv->ce_reactivation_timer = g_timeout_add(CE_REACTIVATION_TIMEOUT_MS,
        nci_adapter_ce_reactivation_timeout, self);
}

static
void
nci_adapter_activation(
    NciAdapter* self,
    const NciIntfActivationNtf* ntf)
{
    NfcAdapter* adapter = NFC_ADAPTER(self);
    NciAdapterPriv* priv = self->priv;
    NciCore* nci = self->nci;

    /* Any activation stops CE reactivation timer if it's running */
    gutil_source_clear(&priv->ce_reactivation_timer);

    /* Update the adapter state */
    switch (priv->internal_state) {
    case NCI_ADAPTER_IDLE:
        /* Continue to object detection */
        break;
    case NCI_ADAPTER_HAVE_TARGET:
        nci_adapter_set_internal_state(priv, NCI_ADAPTER_IDLE);
        nci_adapter_drop_target(self);
        /* Continue to object detection */
        break;
    case NCI_ADAPTER_HAVE_INITIATOR:
        if (nci_adapter_intf_info_matches(priv->active_intf, ntf)) {
            if (priv->host) {
                GDEBUG("CE host spontaneously reactivated");
                nci_adapter_set_internal_state(priv,
                    NCI_ADAPTER_REACTIVATED_CE);
                nfc_initiator_reactivated(priv->initiator);
            } else {
                GDEBUG("Keeping initiator alive");
            }
        } else {
            GDEBUG("Different initiator has arrived, dropping the old one");
            nci_adapter_set_internal_state(priv, NCI_ADAPTER_IDLE);
            nci_adapter_drop_initiator(self);
            /* Continue to object detection */
        }
        break;
    case NCI_ADAPTER_REACTIVATING_CE:
    case NCI_ADAPTER_REACTIVATED_CE:
        if (nci_adapter_intf_info_matches(priv->active_intf, ntf)) {
            if (priv->internal_state == NCI_ADAPTER_REACTIVATED_CE) {
                GDEBUG("Keeping CE initiator alive");
            } else {
                GDEBUG("CE initiator reactivated");
                nci_adapter_set_internal_state(priv,
                    NCI_ADAPTER_REACTIVATED_CE);
            }
            nfc_initiator_reactivated(priv->initiator);
        } else {
            GDEBUG("Different initiator has arrived, dropping the old one");
            nci_adapter_set_internal_state(priv, NCI_ADAPTER_IDLE);
            nci_adapter_drop_initiator(self);
            /* Continue to object detection */
        }
        break;
    case NCI_ADAPTER_REACTIVATING_TARGET:
        if (nci_adapter_intf_info_matches(priv->active_intf, ntf)) {
            GDEBUG("Target reactivated");
            nci_adapter_set_internal_state(priv, NCI_ADAPTER_HAVE_TARGET);
            nfc_target_reactivated(self->target);
        } else {
            GDEBUG("Different tag has arrived, dropping the old one");
            nci_adapter_set_internal_state(priv, NCI_ADAPTER_IDLE);
            nci_adapter_drop_target(self);
            /* Continue to object detection */
        }
        break;
    }

    /* Object detection logic */
    if (!self->target && !priv->initiator) {
        NfcTarget* target = self->target = nci_target_new(self, ntf);

        if (target) {
            nci_adapter_set_internal_state(priv, NCI_ADAPTER_HAVE_TARGET);

            /* Check if it's a peer interface */
            if (!nci_adapter_create_peer_initiator(self, target, ntf)) {
               /* Otherwise assume a tag */
                nci_adapter_intf_info_free(priv->active_intf);
                priv->active_intf = nci_adapter_intf_info_new(ntf);
                if (!nci_adapter_create_known_tag(self, target, ntf)) {
                    NfcParamPoll poll;

                    nci_adapter_set_active_tag(priv,
                        nfc_adapter_add_other_tag2(adapter, target,
                            nci_adapter_get_mode_param(&poll, ntf)));
                }
            }
        } else {
            /* Try initiator then */
            NfcInitiator* initiator = nci_initiator_new(self, ntf);

            if (initiator) {
                if (nci_adapter_create_peer_target(self, initiator, ntf) ||
                    nci_adapter_create_host(self, initiator, ntf)) {
                    /* Keep the initiator */
                    priv->initiator = initiator;
                    nci_adapter_intf_info_free(priv->active_intf);
                    priv->active_intf = nci_adapter_intf_info_new(ntf);
                    nci_adapter_set_internal_state(priv,
                        NCI_ADAPTER_HAVE_INITIATOR);
                } else {
                    nfc_initiator_unref(initiator);
                }
            }
        }
    }

    /* Start periodic presence checks */
    if (nci_adapter_need_presence_checks(self)) {
        if (!priv->presence_check_timer) {
            priv->presence_check_timer = g_timeout_add(PRESENCE_CHECK_PERIOD_MS,
                nci_adapter_presence_check_timer, self);
        }
    } else {
        gutil_source_clear(&priv->presence_check_timer);
    }

    /* If we don't know what this is, switch back to DISCOVERY */
    if (!self->target && !priv->initiator) {
        GDEBUG("No idea what this is");
        nci_core_set_state(nci, NCI_RFST_IDLE);
    }
}

static
void
nci_adapter_deactivation(
    NciAdapter* self)
{
    NciAdapterPriv* priv = self->priv;

    /* Update the adapter state */
    switch (priv->internal_state) {
    case NCI_ADAPTER_REACTIVATING_TARGET:
        break;
    case NCI_ADAPTER_REACTIVATING_CE:
        /* Most likely a reset to lock the CE tech */
        break;
    case NCI_ADAPTER_REACTIVATED_CE:
        nci_adapter_set_internal_state(priv, NCI_ADAPTER_REACTIVATING_CE);
        nci_adapter_start_ce_reactivation_timer(self);
        break;
    case NCI_ADAPTER_HAVE_INITIATOR:
        if (priv->host) {
            NCI_TECH ce_tech = NCI_TECH_NONE;

            /* Lock the card emulation tech */
            switch (priv->initiator->technology) {
            case NFC_TECHNOLOGY_A:
                ce_tech = NCI_TECH_A_LISTEN;
                break;
            case NFC_TECHNOLOGY_B:
                ce_tech = NCI_TECH_B_LISTEN;
                break;
            case NFC_TECHNOLOGY_F:
            case NFC_TECHNOLOGY_UNKNOWN:
                break;
            }

            nci_adapter_set_internal_state(priv, NCI_ADAPTER_REACTIVATING_CE);
            nci_adapter_start_ce_reactivation_timer(self);

            /*
             * The same technology must be used for reactivation, otherwise
             * the peer may not (and most likely won't) recognize us as the
             * came card.
             */
            if (ce_tech) {
                const NCI_TECH tech = priv->active_techs & ce_tech;

                priv->active_tech_mask = ce_tech;
                nci_core_set_tech(self->nci, tech);
            }
            break;
        }
        /* fallthrough */
    case NCI_ADAPTER_IDLE:
    case NCI_ADAPTER_HAVE_TARGET:
        nci_adapter_set_internal_state(priv, NCI_ADAPTER_IDLE);
        nci_adapter_drop_all(self);
        break;
    }
}

/*==========================================================================*
 * NCI core events
 *==========================================================================*/

static
void
nci_adapter_nci_intf_activated(
    NciCore* nci,
    const NciIntfActivationNtf* ntf,
    void* user_data)
{
    NciAdapter* self = THIS(user_data);

    g_object_ref(self);
    nci_adapter_activation(self, ntf);
    g_object_unref(self);
}

static
void
nci_adapter_nci_next_state_changed(
    NciCore* nci,
    void* user_data)
{
    NciAdapter* self = THIS(user_data);

    GET_THIS_CLASS(self)->next_state_changed(self);
}

static
void
nci_adapter_nci_current_state_changed(
    NciCore* nci,
    void* user_data)
{
    NciAdapter* self = THIS(user_data);

    GET_THIS_CLASS(self)->current_state_changed(self);
}

static
void
nci_adapter_nci_param_changed(
    NciCore* nci,
    NCI_CORE_PARAM key,
    void* user_data)
{
    if (key == NCI_CORE_PARAM_LA_NFCID1) {
        nfc_adapter_param_change_notify(NFC_ADAPTER(user_data),
            NFC_ADAPTER_PARAM_LA_NFCID1);
    }
}

/*==========================================================================*
 * Interface
 *==========================================================================*/

void
nci_adapter_init_base(
    NciAdapter* self,
    NciHalIo* io)
{
    NciAdapterPriv* priv = self->priv;

    self->nci = nci_core_new(io);
    priv->active_techs = priv->supported_techs = nci_core_get_tech(self->nci);
    priv->nci_event_id[CORE_EVENT_CURRENT_STATE] =
        nci_core_add_current_state_changed_handler(self->nci,
            nci_adapter_nci_current_state_changed, self);
    priv->nci_event_id[CORE_EVENT_NEXT_STATE] =
        nci_core_add_next_state_changed_handler(self->nci,
            nci_adapter_nci_next_state_changed, self);
    priv->nci_event_id[CORE_EVENT_INTF_ACTIVATED] =
        nci_core_add_intf_activated_handler(self->nci,
            nci_adapter_nci_intf_activated, self);
    priv->nci_event_id[CORE_EVENT_PARAMS_CHANGED] =
        nci_core_add_params_change_handler(self->nci,
            nci_adapter_nci_param_changed, self);
}

/*
 * This is supposed to be called from finalize method of the derived class
 * to make sure that NciCore is freed before NciHalIo in cases when NciHalIo
 * is allocated dynamically. Note that in that case it will be called twice,
 * once by the derived class and once by nci_adapter_finalize.
 */
void
nci_adapter_finalize_core(
    NciAdapter* self)
{
    NciAdapterPriv* priv = self->priv;

    gutil_source_clear(&priv->mode_check_id);
    if (self->nci) {
        nci_core_remove_all_handlers(self->nci, priv->nci_event_id);
        nci_core_free(self->nci);
        self->nci = NULL;
    }
}

gboolean
nci_adapter_reactivate(
    NciAdapter* self,
    NfcTarget* target)
{
    if (self && self->target == target && target) {
        NciAdapterPriv* priv = self->priv;
        NciCore* nci = self->nci;

        if (priv->active_intf &&
            priv->internal_state == NCI_ADAPTER_HAVE_TARGET && nci &&
            ((nci->current_state == NCI_RFST_POLL_ACTIVE &&
              nci->next_state == NCI_RFST_POLL_ACTIVE) ||
             (nci->current_state == NCI_RFST_LISTEN_ACTIVE &&
              nci->next_state == NCI_RFST_LISTEN_ACTIVE))) {
            GDEBUG("Reactivating the interface");
            nci_adapter_set_internal_state(priv,
                NCI_ADAPTER_REACTIVATING_TARGET);
            /* Stop presence checks for the time being */
            gutil_source_clear(&priv->presence_check_timer);
            /* Switch to discovery and expect the same target to reappear */
            nci_core_set_state(nci, NCI_RFST_DISCOVERY);
            return TRUE;
        }
    }
    GWARN("Can't reactivate the tag in this state");
    return FALSE;
}

void
nci_adapter_deactivate_target(
    NciAdapter* self,
    NfcTarget* target)
{
    if (self && self->target == target && target) {
        nci_adapter_drop_target(self);
        if (self->parent.powered) {
            nci_core_set_state(self->nci, NCI_RFST_DISCOVERY);
        }
    }
}

void
nci_adapter_deactivate_initiator(
    NciAdapter* self,
    NfcInitiator* initiator)
{
    if (self && initiator) {
        NciAdapterPriv* priv = self->priv;

        if (priv->initiator == initiator) {
            nci_adapter_drop_initiator(self);
            if (self->parent.powered) {
                nci_core_set_state(self->nci, NCI_RFST_DISCOVERY);
            }
        }
    }
}

/*==========================================================================*
 * Methods
 *==========================================================================*/

static
gboolean
nci_adapter_submit_mode_request(
    NfcAdapter* adapter,
    NFC_MODE mode)
{
    NciAdapter* self = THIS(adapter);
    NciAdapterPriv* priv = self->priv;
    NCI_OP_MODE op_mode = NFC_OP_MODE_NONE;

    if (mode & NFC_MODE_READER_WRITER) {
        op_mode |= (NFC_OP_MODE_RW | NFC_OP_MODE_POLL);
    }
    if (mode & NFC_MODE_P2P_INITIATOR) {
        op_mode |= (NFC_OP_MODE_PEER | NFC_OP_MODE_POLL);
    }
    if (mode & NFC_MODE_P2P_TARGET) {
        op_mode |= (NFC_OP_MODE_PEER | NFC_OP_MODE_LISTEN);
    }
    if (mode & NFC_MODE_CARD_EMILATION) {
        op_mode |= (NFC_OP_MODE_CE | NFC_OP_MODE_LISTEN);
    }

    priv->desired_mode = mode;
    priv->mode_change_pending = TRUE;
    nci_core_set_op_mode(self->nci, op_mode);
    if (op_mode != NFC_OP_MODE_NONE && adapter->powered) {
        nci_core_set_state(self->nci, NCI_RFST_DISCOVERY);
    }
    nci_adapter_schedule_mode_check(self);
    return TRUE;
}

static
void
nci_adapter_cancel_mode_request(
    NfcAdapter* adapter)
{
    NciAdapter* self = THIS(adapter);
    NciAdapterPriv* priv = self->priv;

    priv->mode_change_pending = FALSE;
    nci_adapter_schedule_mode_check(self);
}

static
void
nci_adapter_current_state_changed(
    NciAdapter* self)
{
    nci_adapter_state_check(self);
    nci_adapter_mode_check(self);
}

static
void
nci_adapter_next_state_changed(
    NciAdapter* self)
{
    NciAdapterPriv* priv = self->priv;
    NciCore* nci = self->nci;

    switch (nci->next_state) {
    case NCI_RFST_IDLE:
        if (nci->current_state > NCI_RFST_IDLE) {
            nci_adapter_deactivation(self);
        }
        break;
    case NCI_RFST_DISCOVERY:
        if (nci->current_state != NCI_RFST_IDLE) {
            nci_adapter_deactivation(self);
        }
        break;
    case NCI_RFST_W4_ALL_DISCOVERIES:
    case NCI_RFST_W4_HOST_SELECT:
    case NCI_RFST_POLL_ACTIVE:
    case NCI_RFST_LISTEN_ACTIVE:
    case NCI_RFST_LISTEN_SLEEP:
        break;
    default:
        nci_adapter_set_internal_state(priv, NCI_ADAPTER_IDLE);
        nci_adapter_drop_all(self);
        break;
    }
    nci_adapter_state_check(self);
    nci_adapter_mode_check(self);
}

static
NFC_TECHNOLOGY
nci_adapter_get_supported_techs(
    NfcAdapter* adapter)
{
    NciAdapter* self = THIS(adapter);
    NciAdapterPriv* priv = self->priv;
    NFC_TECHNOLOGY techs = NFC_TECHNOLOGY_UNKNOWN;

    if (priv->supported_techs & NCI_TECH_A) techs |= NFC_TECHNOLOGY_A;
    if (priv->supported_techs & NCI_TECH_B) techs |= NFC_TECHNOLOGY_B;
    if (priv->supported_techs & NCI_TECH_F) techs |= NFC_TECHNOLOGY_F;
    return techs;
}

static
void
nci_adapter_set_allowed_techs(
    NfcAdapter* adapter,
    NFC_TECHNOLOGY techs)
{
    NciAdapter* self = THIS(adapter);
    NciAdapterPriv* priv = self->priv;
    NCI_TECH affected_techs = NCI_TECH_A | NCI_TECH_B | NCI_TECH_F;

    priv->active_techs = priv->supported_techs & ~affected_techs;
    if (techs & NFC_TECHNOLOGY_A) {
        priv->active_techs |= priv->supported_techs & NCI_TECH_A;
    }
    if (techs & NFC_TECHNOLOGY_B) {
        priv->active_techs |= priv->supported_techs & NCI_TECH_B;
    }
    if (techs & NFC_TECHNOLOGY_F) {
        priv->active_techs |= priv->supported_techs & NCI_TECH_F;
    }
    nci_core_set_tech(self->nci, priv->active_techs & priv->active_tech_mask);
}

static
const NFC_ADAPTER_PARAM*
nci_adapter_list_params(
    NfcAdapter* adapter)
{
    NciAdapter* self = THIS(adapter);
    NciAdapterPriv* priv = self->priv;
    static const NFC_ADAPTER_PARAM nci_adapter_param_ids[] = {
        NFC_ADAPTER_PARAM_LA_NFCID1,
        NFC_ADAPTER_PARAM_NONE
    };

    /* Allocate the list on demand */
    if (!priv->supported_params) {
        priv->supported_params =
            nfc_adapter_param_list_merge(nci_adapter_param_ids,
                 NFC_ADAPTER_CLASS(PARENT_CLASS)->list_params(adapter),
                 NULL);
    }
    return priv->supported_params;
}

static
NfcAdapterParamValue*
nci_adapter_get_param(
    NfcAdapter* adapter,
    NFC_ADAPTER_PARAM id) /* Caller frees the result with g_free() */
{
    if (id == NFC_ADAPTER_PARAM_LA_NFCID1) {
        NciAdapter* self = THIS(adapter);
        NciCoreParamValue value;

        memset(&value, 0, sizeof(value));
        if (nci_core_get_param(self->nci, NCI_CORE_PARAM_LA_NFCID1, &value)) {
            NfcAdapterParamValue* out = g_new0(NfcAdapterParamValue, 1);

            /* NCI_CORE_PARAM_LA_NFCID1 => NFC_ADAPTER_PARAM_LA_NFCID1 */
            out->nfcid1.len = MIN(value.nfcid1.len, sizeof(out->nfcid1.bytes));
            memcpy(out->nfcid1.bytes, value.nfcid1.bytes, value.nfcid1.len);
            return out;
        }
    }
    return NFC_ADAPTER_CLASS(PARENT_CLASS)->get_param(adapter, id);
}

static
void
nci_adapter_set_params(
    NfcAdapter* adapter,
    const NfcAdapterParam* const* params, /* NULL terminated list */
    gboolean reset) /* Reset all params that are not being set */
{
    NciAdapter* self = THIS(adapter);
    const NfcAdapterParamValue* set_la_nfcid1 = NULL;
    const NfcAdapterParam* const* ptr = params;

    while (*ptr) {
        const NfcAdapterParam* p = *ptr++;

        if (p->id == NFC_ADAPTER_PARAM_LA_NFCID1) {
            set_la_nfcid1 = &p->value;
        }
    }

    if (set_la_nfcid1) {
        const NciCoreParam* nci_params[2];
        NciCoreParam nci_la_nfcid1;

        /* NFC_ADAPTER_PARAM_LA_NFCID1 => NCI_CORE_PARAM_LA_NFCID1 */
        memset(&nci_la_nfcid1, 0, sizeof(nci_la_nfcid1));
        nci_la_nfcid1.key = NCI_CORE_PARAM_LA_NFCID1;
        if (set_la_nfcid1->nfcid1.len) {
            const NfcId1* src = &set_la_nfcid1->nfcid1;
            NciNfcid1* dest = &nci_la_nfcid1.value.nfcid1;

            dest->len = MIN(src->len, sizeof(dest->bytes));
            memcpy(dest->bytes, src->bytes, dest->len);
        }
        nci_params[0] = &nci_la_nfcid1;
        nci_params[1] = NULL;
        nci_core_set_params(self->nci, nci_params, reset);
    } else if (reset) {
        nci_core_set_params(self->nci, NULL, reset);
    }

    NFC_ADAPTER_CLASS(PARENT_CLASS)->set_params(adapter, params, reset);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nci_adapter_init(
    NciAdapter* self)
{
    NfcAdapter* adapter = NFC_ADAPTER(self);
    NciAdapterPriv* priv = G_TYPE_INSTANCE_GET_PRIVATE(self, THIS_TYPE,
        NciAdapterPriv);

    self->priv = priv;
    priv->active_tech_mask = NCI_TECH_ALL;
    priv->internal_state = NCI_ADAPTER_IDLE;
    adapter->supported_modes = NFC_MODE_READER_WRITER |
        NFC_MODE_P2P_INITIATOR | NFC_MODE_P2P_TARGET |
        NFC_MODE_CARD_EMILATION;
    adapter->supported_tags = NFC_TAG_TYPE_MIFARE_ULTRALIGHT;
    adapter->supported_protocols = NFC_PROTOCOL_T2_TAG |
        NFC_PROTOCOL_T4A_TAG | NFC_PROTOCOL_T4B_TAG |
        NFC_PROTOCOL_NFC_DEP;
}

static
void
nci_adapter_dispose(
    GObject* object)
{
    nci_adapter_drop_all(THIS(object));
    G_OBJECT_CLASS(PARENT_CLASS)->dispose(object);
}

static
void
nci_adapter_finalize(
    GObject* object)
{
    NciAdapter* self = THIS(object);
    NciAdapterPriv* priv = self->priv;

    nci_adapter_set_active_tag(priv, NULL);
    nci_adapter_set_active_peer(priv, NULL);
    nci_adapter_set_active_host(priv, NULL);
    gutil_source_clear(&priv->ce_reactivation_timer);
    gutil_source_clear(&priv->presence_check_timer);
    g_free(priv->supported_params);
    nci_adapter_finalize_core(self);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
nci_adapter_class_init(
    NciAdapterClass* klass)
{
    NfcAdapterClass* adapter_class = NFC_ADAPTER_CLASS(klass);
    GObjectClass* object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(NciAdapterPriv));
    klass->current_state_changed = nci_adapter_current_state_changed;
    klass->next_state_changed = nci_adapter_next_state_changed;
    adapter_class->submit_mode_request = nci_adapter_submit_mode_request;
    adapter_class->cancel_mode_request = nci_adapter_cancel_mode_request;
    adapter_class->get_supported_techs = nci_adapter_get_supported_techs;
    adapter_class->set_allowed_techs = nci_adapter_set_allowed_techs;
    adapter_class->list_params = nci_adapter_list_params;
    adapter_class->get_param = nci_adapter_get_param;
    adapter_class->set_params = nci_adapter_set_params;
    object_class->dispose = nci_adapter_dispose;
    object_class->finalize = nci_adapter_finalize;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
