/*
 * Copyright (C) 2019-2021 Jolla Ltd.
 * Copyright (C) 2019-2021 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
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

#include "nci_plugin_p.h"
#include "nci_plugin_log.h"
#include "nci_adapter_impl.h"

#include <nci_core.h>

#include <nfc_target_impl.h>

#define T2T_CMD_READ (0x30)

enum {
    EVENT_DATA_PACKET,
    EVENT_COUNT
};

typedef NfcTargetClass NciTargetClass;
typedef struct nci_target NciTarget;

typedef struct nci_target_presence_check {
    NciTargetPresenseCheckFunc done;
    void* user_data;
} NciTargetPresenceCheck;

typedef
guint
(*NciTargetPresenceCheckFunc)(
    NciTarget* self,
    NciTargetPresenceCheck* check);

typedef
gboolean
(*NciTargetTransmitFinishFunc)(
    NfcTarget* target,
    const guint8* payload,
    guint len);

struct nci_target {
    NfcTarget target;
    NciAdapter* adapter;
    gulong event_id[EVENT_COUNT];
    guint send_in_progress;
    gboolean transmit_in_progress;
    GBytes* pending_reply; /* Reply arrived before send has completed */
    NciTargetPresenceCheckFunc presence_check_fn;
    NciTargetTransmitFinishFunc transmit_finish_fn;
};

GType nci_target_get_type(void) G_GNUC_INTERNAL;
#define PARENT_CLASS nci_target_parent_class
#define THIS_TYPE (nci_target_get_type())
#define THIS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, NciTarget))
G_DEFINE_TYPE(NciTarget, nci_target, NFC_TYPE_TARGET)

static
NciTarget*
nci_target_new_with_technology(
    NFC_TECHNOLOGY technology)
{
    NciTarget* self = g_object_new(THIS_TYPE, NULL);

    self->target.technology = technology;
    return self;
}

static
NciTargetPresenceCheck*
nci_target_presence_check_new(
    NciTargetPresenseCheckFunc fn,
    void* user_data)
{
    NciTargetPresenceCheck* check =
        g_slice_new(NciTargetPresenceCheck);

    check->done = fn;
    check->user_data = user_data;
    return check;
}

static
void
nci_target_presence_check_free(
    NciTargetPresenceCheck* check)
{
    g_slice_free(NciTargetPresenceCheck, check);
}

static
void
nci_target_presence_check_free1(
    gpointer data)
{
    nci_target_presence_check_free(data);
}

static
void
nci_target_cancel_send(
    NciTarget* self)
{
    if (self->send_in_progress) {
        if (self->adapter) {
            nci_core_cancel(self->adapter->nci, self->send_in_progress);
        }
        self->send_in_progress = 0;
        if (self->pending_reply) {
            g_bytes_unref(self->pending_reply);
            self->pending_reply = NULL;
        }
    }
}

static
void
nci_target_drop_adapter(
    NciTarget* self)
{
    if (self->adapter) {
        NciAdapter* adapter = self->adapter;

        nci_target_cancel_send(self);
        nci_core_remove_all_handlers(adapter->nci, self->event_id);
        g_object_remove_weak_pointer(G_OBJECT(adapter), (gpointer*)
            &self->adapter);
        self->adapter = NULL;
    }
}

static
void
nci_target_finish_transmit(
    NciTarget* self,
    const guint8* payload,
    guint len)
{
    NfcTarget* target = &self->target;

    self->transmit_in_progress = FALSE;
    if (!self->transmit_finish_fn ||
        !self->transmit_finish_fn(target, payload, len)) {
        nfc_target_transmit_done(target, NFC_TRANSMIT_STATUS_ERROR, NULL, 0);
    }
}

static
void
nci_target_data_sent(
    NciCore* nci,
    gboolean success,
    void* user_data)
{
    NciTarget* self = THIS(user_data);

    GASSERT(self->send_in_progress);
    self->send_in_progress = 0;

    if (self->pending_reply) {
        gsize len;
        GBytes* reply = self->pending_reply;
        const guint8* payload = g_bytes_get_data(reply, &len);

        /* We have been waiting for this send to complete */
        GDEBUG("Send completed");
        self->pending_reply = NULL;
        nci_target_finish_transmit(self, payload, len);
        g_bytes_unref(reply);
    }
}

static
void
nci_target_data_packet_handler(
    NciCore* nci,
    guint8 cid,
    const void* data,
    guint len,
    void* user_data)
{
    NciTarget* self = THIS(user_data);

    if (cid == NCI_STATIC_RF_CONN_ID && self->transmit_in_progress &&
        !self->pending_reply) {
        if (G_UNLIKELY(self->send_in_progress)) {
            /*
             * Due to multi-threaded nature of pn547 driver and services,
             * incoming reply transactions sometimes get handled before
             * send completion callback has been invoked. Postpone transfer
             * completion until then.
             */
            GDEBUG("Waiting for send to complete");
            self->pending_reply = g_bytes_new(data, len);
        } else {
            nci_target_finish_transmit(self, data, len);
        }
    } else {
        GDEBUG("Unhandled data packet, cid=0x%02x %u byte(s)", cid, len);
    }
}

static
void
nci_target_presence_check_complete(
    NfcTarget* target,
    NFC_TRANSMIT_STATUS status,
    const void* data,
    guint len,
    void* user_data)
{
    NciTargetPresenceCheck* check = user_data;

    check->done(target, status == NFC_TRANSMIT_STATUS_OK, check->user_data);
}

static
guint
nci_target_presence_check_t2(
    NciTarget* self,
    NciTargetPresenceCheck* check)
{
    static const guint8 cmd_data[] = { T2T_CMD_READ, 0x00 };

    return nfc_target_transmit(&self->target, cmd_data, sizeof(cmd_data),
        NULL, nci_target_presence_check_complete,
        nci_target_presence_check_free1, check);
}

static
guint
nci_target_presence_check_t4(
    NciTarget* self,
    NciTargetPresenceCheck* check)
{
    return nfc_target_transmit(&self->target, NULL, 0,
        NULL, nci_target_presence_check_complete,
        nci_target_presence_check_free1, check);
}

static
gboolean
nci_target_transmit_finish_frame(
    NfcTarget* target,
    const guint8* payload,
    guint len)
{
    if (len > 0) {
        const guint8 status = payload[len - 1];

        /*
         * 8.2 Frame RF Interface
         * 8.2.1.2 Data from RF to the DH
         */
        if (status == NCI_STATUS_OK) {
            nfc_target_transmit_done(target, NFC_TRANSMIT_STATUS_OK,
                payload, len - 1);
            return TRUE;
        }
        GDEBUG("Transmission status 0x%02x", status);
    }
    return FALSE;
}

static
gboolean
nci_target_transmit_finish_iso_dep(
    NfcTarget* target,
    const guint8* payload,
    guint len)
{
    /*
     * 8.3 ISO-DEP RF Interface
     * 8.3.1.2 Data from RF to the DH
     */
    nfc_target_transmit_done(target, NFC_TRANSMIT_STATUS_OK, payload, len);
    return TRUE;
}

static
gboolean
nci_target_transmit_finish_nfc_dep(
    NfcTarget* target,
    const guint8* payload,
    guint len)
{
    /*
     * 8.4 NFC-DEP RF Interface
     * 8.4.1.2 Data from RF to the DH
     */
    nfc_target_transmit_done(target, NFC_TRANSMIT_STATUS_OK, payload, len);
    return TRUE;
}

/*==========================================================================*
 * Interface
 *==========================================================================*/

NfcTarget*
nci_target_new(
    NciAdapter* adapter,
    const NciIntfActivationNtf* ntf)
{
    NFC_TECHNOLOGY tech = NFC_TECHNOLOGY_UNKNOWN;

    switch (ntf->mode) {
    case NCI_MODE_PASSIVE_POLL_A:
    case NCI_MODE_ACTIVE_POLL_A:
        tech = NFC_TECHNOLOGY_A;
        break;
    case NCI_MODE_PASSIVE_POLL_B:
        tech = NFC_TECHNOLOGY_B;
        break;
    case NCI_MODE_PASSIVE_POLL_F:
    case NCI_MODE_ACTIVE_POLL_F:
        tech = NFC_TECHNOLOGY_F;
        break;
    case NCI_MODE_PASSIVE_POLL_V:
    case NCI_MODE_PASSIVE_LISTEN_V:
    case NCI_MODE_PASSIVE_LISTEN_A:
    case NCI_MODE_PASSIVE_LISTEN_B:
    case NCI_MODE_PASSIVE_LISTEN_F:
    case NCI_MODE_ACTIVE_LISTEN_A:
    case NCI_MODE_ACTIVE_LISTEN_F:
        break;
    }

    if (tech != NFC_TECHNOLOGY_UNKNOWN) {
        NFC_PROTOCOL protocol = NFC_PROTOCOL_UNKNOWN;
        NciTargetPresenceCheckFunc presence_check = NULL;

        switch (ntf->protocol) {
        case NCI_PROTOCOL_T1T:
            protocol = NFC_PROTOCOL_T1_TAG;
            break;
        case NCI_PROTOCOL_T2T:
            protocol = NFC_PROTOCOL_T2_TAG;
            presence_check = nci_target_presence_check_t2;
            break;
        case NCI_PROTOCOL_T3T:
            protocol = NFC_PROTOCOL_T3_TAG;
            break;
        case NCI_PROTOCOL_ISO_DEP:
            presence_check = nci_target_presence_check_t4;
            switch (tech) {
            case NFC_TECHNOLOGY_A:
                protocol = NFC_PROTOCOL_T4A_TAG;
                break;
            case NFC_TECHNOLOGY_B:
                protocol = NFC_PROTOCOL_T4B_TAG;
                break;
            default:
                GDEBUG("Unexpected ISO_DEP technology");
                break;
            }
            break;
        case NCI_PROTOCOL_NFC_DEP:
            protocol = NFC_PROTOCOL_NFC_DEP;
            break;
        default:
            GDEBUG("Unsupported protocol 0x%02x", ntf->protocol);
            break;
        }

        if (protocol != NFC_PROTOCOL_UNKNOWN) {
            NciTargetTransmitFinishFunc transmit_finish = NULL;
            int tx_timeout = -1;

            switch (ntf->rf_intf) {
            case NCI_RF_INTERFACE_FRAME:
                switch (ntf->protocol) {
                case NCI_PROTOCOL_NFC_DEP:
                    GDEBUG("Frame interface not supported for NFC-DEP");
                    break;
                case NCI_PROTOCOL_ISO_DEP:
                    GDEBUG("Frame interface not supported for ISO-DEP");
                    break;
                default:
                    transmit_finish = nci_target_transmit_finish_frame;
                    break;
                }
                break;
            case NCI_RF_INTERFACE_ISO_DEP:
                transmit_finish = nci_target_transmit_finish_iso_dep;
                break;
            case NCI_RF_INTERFACE_NFC_DEP:
                tx_timeout = 0; /* Rely on CORE_INTERFACE_ERROR_NTF */
                transmit_finish = nci_target_transmit_finish_nfc_dep;
                break;
            default:
                GDEBUG("Unsupported RF interface 0x%02x", ntf->rf_intf);
                break;
            }

            if (transmit_finish) {
                NciTarget* self = nci_target_new_with_technology(tech);
                NfcTarget* target = &self->target;

                target->protocol = protocol;
                self->adapter = adapter;
                self->presence_check_fn = presence_check;
                self->transmit_finish_fn = transmit_finish;
                nfc_target_set_transmit_timeout(target, tx_timeout);
                g_object_add_weak_pointer(G_OBJECT(adapter),
                    (gpointer*) &self->adapter);
                self->event_id[EVENT_DATA_PACKET] =
                    nci_core_add_data_packet_handler(adapter->nci,
                        nci_target_data_packet_handler, self);
                return target;
            }
        }
    }
    return NULL;
}

guint
nci_target_presence_check(
    NfcTarget* target,
    NciTargetPresenseCheckFunc fn,
    void* user_data)
{
    if (G_LIKELY(target)) {
        NciTarget* self = THIS(target);

        if (self && self->presence_check_fn) {
            NciTargetPresenceCheck* check =
                nci_target_presence_check_new(fn, user_data);
            const guint id = self->presence_check_fn(self, check);

            if (id) {
                return id;
            }
            nci_target_presence_check_free(check);
        }
    }
    return 0;
}

/*==========================================================================*
 * Methods
 *==========================================================================*/

static
gboolean
nci_target_transmit(
    NfcTarget* target,
    const void* data,
    guint len)
{
    NciTarget* self = THIS(target);
    NciAdapter* adapter = self->adapter;

    GASSERT(!self->send_in_progress);
    GASSERT(!self->transmit_in_progress);
    if (adapter) {
        GBytes* bytes = g_bytes_new(data, len);

        self->send_in_progress = nci_core_send_data_msg(adapter->nci,
            NCI_STATIC_RF_CONN_ID, bytes, nci_target_data_sent,
            NULL, self);
        g_bytes_unref(bytes);
        if (self->send_in_progress) {
            self->transmit_in_progress = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

static
void
nci_target_cancel_transmit(
    NfcTarget* target)
{
    NciTarget* self = THIS(target);

    self->transmit_in_progress = FALSE;
    nci_target_cancel_send(self);
}

static
void
nci_target_deactivate(
    NfcTarget* target)
{
    nci_adapter_deactivate_target(THIS(target)->adapter, target);
}

static
void
nci_target_gone(
    NfcTarget* target)
{
    nci_target_drop_adapter(THIS(target));
    NFC_TARGET_CLASS(PARENT_CLASS)->gone(target);
}

static
gboolean
nci_target_reactivate(
    NfcTarget* target)
{
    NciTarget* self = THIS(target);

    return self->adapter && nci_adapter_reactivate(self->adapter, target);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nci_target_init(
    NciTarget* self)
{
}

static
void
nci_target_finalize(
    GObject* object)
{
    nci_target_drop_adapter(THIS(object));
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
nci_target_class_init(
    NfcTargetClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = nci_target_finalize;
    klass->deactivate = nci_target_deactivate;
    klass->transmit = nci_target_transmit;
    klass->cancel_transmit = nci_target_cancel_transmit;
    klass->gone = nci_target_gone;
    klass->reactivate = nci_target_reactivate;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
