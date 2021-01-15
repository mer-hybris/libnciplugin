/*
 * Copyright (C) 2020 Jolla Ltd.
 * Copyright (C) 2020 Slava Monich <slava.monich@jolla.com>
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

#include <nfc_initiator_impl.h>

enum {
    EVENT_DATA_PACKET,
    EVENT_COUNT
};

typedef NfcInitiatorClass NciInitiatorClass;
typedef struct nci_initiator {
    NfcInitiator initiator;
    NciAdapter* adapter;
    gulong event_id[EVENT_COUNT];
    guint response_in_progress;
} NciInitiator;

GType nci_initiator_get_type(void) G_GNUC_INTERNAL;
#define PARENT_CLASS nci_initiator_parent_class
#define THIS_TYPE (nci_initiator_get_type())
#define THIS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, NciInitiator))
G_DEFINE_TYPE(NciInitiator, nci_initiator, NFC_TYPE_INITIATOR)

static
NciInitiator*
nci_initiator_new_with_technology(
    NFC_TECHNOLOGY technology)
{
    NciInitiator* self = g_object_new(THIS_TYPE, NULL);

    self->initiator.technology = technology;
    return self;
}

static
void
nci_initiator_cancel_response(
    NciInitiator* self)
{
    if (self->response_in_progress) {
        if (self->adapter) {
            nci_core_cancel(self->adapter->nci, self->response_in_progress);
        }
        self->response_in_progress = 0;
    }
}

static
void
nci_initiator_drop_adapter(
    NciInitiator* self)
{
    if (self->adapter) {
        NciAdapter* adapter = self->adapter;

        nci_initiator_cancel_response(self);
        nci_core_remove_all_handlers(adapter->nci, self->event_id);
        g_object_remove_weak_pointer(G_OBJECT(adapter), (gpointer*)
            &self->adapter);
        self->adapter = NULL;
    }
}

static
void
nci_initiator_data_packet_handler(
    NciCore* nci,
    guint8 cid,
    const void* data,
    guint len,
    void* user_data)
{
    if (cid == NCI_STATIC_RF_CONN_ID) {
        nfc_initiator_transmit(NFC_INITIATOR(user_data), data, len);
    } else {
        GDEBUG("Unhandled data packet, cid=0x%02x %u byte(s)", cid, len);
    }
}

static
void
nci_initiator_response_sent(
    NciCore* nci,
    gboolean success,
    void* user_data)
{
    NciInitiator* self = THIS(user_data);

    GASSERT(self->response_in_progress);
    self->response_in_progress = 0;
    nfc_initiator_response_sent(&self->initiator, success ?
        NFC_TRANSMIT_STATUS_OK : NFC_TRANSMIT_STATUS_ERROR);
}

/*==========================================================================*
 * Interface
 *==========================================================================*/

NfcInitiator*
nci_initiator_new(
    NciAdapter* adapter,
    const NciIntfActivationNtf* ntf)
{
    NFC_TECHNOLOGY tech = NFC_TECHNOLOGY_UNKNOWN;

    switch (ntf->mode) {
    case NCI_MODE_ACTIVE_LISTEN_A:
    case NCI_MODE_PASSIVE_LISTEN_A:
        tech = NFC_TECHNOLOGY_A;
        break;
    case NCI_MODE_PASSIVE_LISTEN_B:
        tech = NFC_TECHNOLOGY_B;
        break;
    case NCI_MODE_ACTIVE_LISTEN_F:
    case NCI_MODE_PASSIVE_LISTEN_F:
        tech = NFC_TECHNOLOGY_F;
        break;
    case NCI_MODE_PASSIVE_POLL_A:
    case NCI_MODE_ACTIVE_POLL_A:
    case NCI_MODE_PASSIVE_POLL_B:
    case NCI_MODE_PASSIVE_POLL_F:
    case NCI_MODE_ACTIVE_POLL_F:
    case NCI_MODE_PASSIVE_POLL_15693:
    case NCI_MODE_PASSIVE_LISTEN_15693:
        break;
    }

    if (tech != NFC_TECHNOLOGY_UNKNOWN) {
        NFC_PROTOCOL protocol = NFC_PROTOCOL_UNKNOWN;

        switch (ntf->protocol) {
        case NCI_PROTOCOL_NFC_DEP:
            protocol = NFC_PROTOCOL_NFC_DEP;
            break;
        case NCI_PROTOCOL_ISO_DEP:
            GDEBUG("Card emulation (ISO-DEP) not supported yet");
            break;
        default:
            GDEBUG("Unsupported initiator protocol 0x%02x", ntf->protocol);
            break;
        }

        if (protocol != NFC_PROTOCOL_UNKNOWN) {
            NciInitiator* self = nci_initiator_new_with_technology(tech);
            NfcInitiator* initiator = &self->initiator;

            initiator->protocol = NFC_PROTOCOL_NFC_DEP;
            self->adapter = adapter;
            g_object_add_weak_pointer(G_OBJECT(adapter),
                (gpointer*) &self->adapter);
            self->event_id[EVENT_DATA_PACKET] =
                nci_core_add_data_packet_handler(adapter->nci,
                    nci_initiator_data_packet_handler, self);
            return initiator;
        }
    }
    return NULL;
}

/*==========================================================================*
 * Methods
 *==========================================================================*/

static
gboolean
nci_initiator_respond(
    NfcInitiator* initiator,
    const void* data,
    guint len)
{
    NciInitiator* self = THIS(initiator);
    NciAdapter* adapter = self->adapter;

    GASSERT(!self->response_in_progress);

    if (adapter) {
        GBytes* bytes = g_bytes_new(data, len);

        self->response_in_progress = nci_core_send_data_msg(adapter->nci,
            NCI_STATIC_RF_CONN_ID, bytes, nci_initiator_response_sent,
            NULL, self);
        g_bytes_unref(bytes);
        if (self->response_in_progress) {
            return TRUE;
        }
    }
    return FALSE;
}

static
void
nci_initiator_deactivate(
    NfcInitiator* initiator)
{
    nci_adapter_deactivate_initiator(THIS(initiator)->adapter, initiator);
}

static
void
nci_initiator_gone(
    NfcInitiator* initiator)
{
    nci_initiator_drop_adapter(THIS(initiator));
    NFC_INITIATOR_CLASS(PARENT_CLASS)->gone(initiator);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nci_initiator_init(
    NciInitiator* self)
{
}

static
void
nci_initiator_finalize(
    GObject* object)
{
    nci_initiator_drop_adapter(THIS(object));
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
nci_initiator_class_init(
    NfcInitiatorClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = nci_initiator_finalize;
    klass->deactivate = nci_initiator_deactivate;
    klass->respond = nci_initiator_respond;
    klass->gone = nci_initiator_gone;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
