/** @file midi_upload.h
 *  @brief Receive a firmware image over USB MIDI into the staging buffer.
 *
 *  The SD card is one way into fw_image; this is the other. A host streams the
 *  image as SysEx together with a slot number and a name, and this class
 *  assembles and checks it. Storing it in that slot on the card, and starting
 *  it, is the caller's job: Poll() reports Event::Complete and the caller
 *  answers the END with FinishEnd() once the card write is done. LIST and
 *  CLEAR also need the card, so they go the same way: Poll() reports the
 *  event and the caller answers with FinishList() or FinishClear().
 *
 *  The protocol is specified in PROTOCOL.md, next to the launcher's README.
 *  In short, every message, both directions, is
 *
 *      F0 7D 43 48 <cmd> <payload...> F7
 *
 *      01 PING                                -> 41 OK version max_chunk slots
 *                                                    features launcher_version
 *      02 BEGIN  total:u28 slot name_len name -> 42 status
 *      03 DATA   offset:u28  packed           -> 43 status  received:u28
 *      04 END    crc32:u35  [flags]           -> 44 status  (after the write)
 *      05 RUN    slot                         -> 45 status  (then starts it)
 *      06 LIST   slot                         -> 46 status slot name_len name
 *                                                    size:u28
 *      07 CLEAR  slot [name_len name]         -> 47 status slot removed
 *
 *  Test launchers 1.4.0 - 1.4.2 had LIST and CLEAR at 05 and 06.
 *
 *  The host sends one message and waits for its reply. Stop-and-wait keeps the
 *  receive side to a single buffer, and USB round trips are short enough that
 *  an image still lands in a few seconds. DATA is addressed by offset, so a
 *  resend after a lost reply is harmless, and the reply always carries how much
 *  has arrived so the host can pick up from there.
 *
 *  RUN starts a slot that is already on the card, as its key would, so a host
 *  can switch firmwares without anyone at the panel. Poll() reports Event::Run
 *  and the caller answers with FinishRun() before it starts the slot.
 */
#pragma once
#include "daisy_seed.h"

/** The launcher's own version. Also in the image as kLauncherTag, which is
 *  how a launcher recognises another one sent to it for self-update. */
#ifndef CHOMPI_LAUNCHER_VERSION_PATCH
#define CHOMPI_LAUNCHER_VERSION_PATCH 0
#endif
#define CHOMPI_LAUNCHER_VERSION_MAJOR 1
#define CHOMPI_LAUNCHER_VERSION_MINOR 5
#define CHOMPI_LAUNCHER_TAG_PREFIX "CHOMPI-LAUNCHER "
#include "chainload.h"
#include <cstring>

namespace chompi
{
    class MidiUpload
    {
      public:
        static constexpr uint8_t  kVersion  = 1;
        /** The launcher's own version, major.minor.patch, after the features
         *  byte in the PING reply. Test build: not a release number yet. */
        static constexpr uint8_t kLauncherVersion[3]
            = {CHOMPI_LAUNCHER_VERSION_MAJOR, CHOMPI_LAUNCHER_VERSION_MINOR,
               CHOMPI_LAUNCHER_VERSION_PATCH};
        /** BEGIN's slot for "this image is a new launcher" (FEATURE_LAUNCHER).
         *  Out of the range of keys; slot 0 is reserved for something else. */
        static constexpr uint8_t kLauncherSlot = 127;
        static constexpr uint32_t kMaxChunk = 2048; /**< raw bytes per DATA */
        static constexpr size_t   kMaxName  = 16;  /**< chars in a slot name */
        /** Most chars of a card filename LIST reports. */
        static constexpr size_t kMaxListName = 40;

        /** What this launcher can do beyond protocol 1's upload, as the last
         *  byte of the PING reply. Older launchers send no such byte. */
        enum Feature : uint8_t
        {
            FEATURE_LIST  = 1 << 0, /**< 06 LIST */
            FEATURE_CLEAR = 1 << 1, /**< 07 CLEAR */
            FEATURE_STAY  = 1 << 2, /**< END flag: store, do not start */
            FEATURE_LAUNCHER = 1 << 3, /**< slot 127: update the launcher */
            FEATURE_RUN   = 1 << 4, /**< 05 RUN */
        };
        static constexpr uint8_t kFeatures
            = FEATURE_LIST | FEATURE_CLEAR | FEATURE_STAY | FEATURE_LAUNCHER | FEATURE_RUN;

        /** END's optional flags byte. */
        static constexpr uint8_t kEndStay = 1 << 0;

        enum Cmd : uint8_t
        {
            PING  = 0x01,
            BEGIN = 0x02,
            DATA  = 0x03,
            END   = 0x04,
            RUN   = 0x05,
            LIST  = 0x06,
            CLEAR = 0x07,
            REPLY = 0x40, /**< or'd into the command being answered */
        };

        enum Status : uint8_t
        {
            OK          = 0,
            BAD_MESSAGE = 1, /**< malformed, or an unknown command */
            BAD_OFFSET  = 2, /**< DATA would leave a gap */
            TOO_BIG     = 3, /**< image or chunk exceeds the buffer */
            BAD_CRC     = 4,
            BAD_IMAGE   = 5, /**< vector table rejected */
            NOT_STARTED = 6, /**< DATA or END without a BEGIN */
            INCOMPLETE  = 7, /**< END before every byte arrived */
            BAD_SLOT    = 8, /**< slot is 0, above the number of slots, or
                                  (RUN) has no firmware in it */
            BAD_NAME    = 9, /**< empty, too long, or a forbidden character */
            NO_CARD      = 10, /**< no card, or it would not mount */
            WRITE_FAILED = 11, /**< the card refused it, or readback differed */
            CARD_FULL    = 12, /**< no room on the card for the image */
            NOT_LAUNCHER = 13, /**< slot 127, but the image is no launcher */
            OTHER_BIN    = 14, /**< another .bin in the card root would be
                                    installed instead of the new launcher */
        };

        /** What Poll() saw, for the caller's LEDs and log. */
        enum class Event
        {
            None,
            Pinged,
            Started,
            Progress,
            Rejected, /**< a message was answered with an error */
            Complete, /**< image whole and checked; END awaits FinishEnd() */
            List,     /**< LIST awaits FinishList() */
            Clear,    /**< CLEAR awaits FinishClear() */
            Run,      /**< RUN for RunSlot(); awaits FinishRun() */
        };

        /** Start USB MIDI and begin listening.
         *  @param image    where the image is assembled (fw_image, in SDRAM)
         *  @param capacity its size in bytes
         *  @param slots    highest slot number a BEGIN may name
         */
        void Init(uint8_t *image, uint32_t capacity, uint8_t slots)
        {
            image_    = image;
            capacity_ = capacity;
            slots_    = slots;

            MidiUsbTransport::Config cfg;
            cfg.periph = MidiUsbTransport::Config::EXTERNAL;
            usb_.Init(cfg);
            usb_.StartRx(RxCallback, this);
        }

        /** Handle at most one received message and answer it. Main loop only:
         *  replies go out from here, never from the USB interrupt. */
        Event Poll()
        {
            /* The transport stops listening for good if its ring ever
               overflows. Cannot happen as we drain it on every packet, but
               there is no other way back if it somehow does. */
            if (!usb_.RxActive())
                usb_.StartRx(RxCallback, this);

            if (!msg_ready_)
                return Event::None;
            __DMB();

            const Event ev = Handle(msg_, msg_len_);

            __DMB();
            msg_ready_ = false; /* hand the buffer back to the interrupt */
            return ev;
        }

        /** An upload is under way: BEGIN accepted, recently heard from, and
         *  not yet stored. */
        bool Active(uint32_t now) const
        {
            return total_ > 0 && !stored_ && now - last_heard_ < kStallMs;
        }

        /** Answer the END that produced Event::Complete, once the image has
         *  been stored (OK) or could not be (NO_CARD, WRITE_FAILED). On OK the
         *  caller then boots it, unless Stay(); on a failure the upload stays
         *  intact, so the host may fix the problem and send END again. */
        void FinishEnd(Status s)
        {
            stored_ = (s == OK);
            ReplyStatus(END, s);
        }

        /** Answer LIST for Slot(): what is on that key, or name "" if
         *  nothing. Characters outside printable ASCII are sent as '?'. */
        void FinishList(Status s, const char *name, uint32_t size)
        {
            if (s != OK)
            {
                ReplyStatus(LIST, s);
                return;
            }
            uint8_t p[2 + 1 + kMaxListName + 4] = {s, query_slot_};
            const size_t len = ListName(name, p + 3);
            p[2] = (uint8_t)len;
            Put7(p + 3 + len, size, 4);
            last_status_ = s;
            Reply(LIST, p, 3 + len + 4);
        }

        /** Whether CLEAR may remove `name`, the file on Slot()'s key: yes if
         *  the CLEAR named no file, or named this one as LIST reports it. */
        bool ClearMatches(const char *name) const
        {
            if (!clear_name_given_)
                return true;
            uint8_t listed[kMaxListName];
            const size_t len = ListName(name, listed);
            return len == clear_name_len_ && memcmp(listed, clear_name_, len) == 0;
        }

        /** Answer CLEAR: OK with whether anything was removed, or why the key
         *  could not be emptied. */
        void FinishClear(Status s, bool removed)
        {
            if (s != OK)
            {
                ReplyStatus(CLEAR, s);
                return;
            }
            const uint8_t p[3] = {s, query_slot_, (uint8_t)(removed ? 1 : 0)};
            last_status_       = s;
            Reply(CLEAR, p, sizeof(p));
        }

        /** The END asked to store only: the launcher stays in the picker. */
        bool Stay() const { return end_flags_ & kEndStay; }

        /** Answer the RUN that produced Event::Run: OK just before the caller
         *  starts the slot, BAD_SLOT if there is nothing in it. */
        void FinishRun(Status s) { ReplyStatus(RUN, s); }

        uint32_t    Received() const { return received_; }
        uint32_t    Total() const { return total_; }
        uint32_t    Crc() const { return crc_; }
        /** The upload's slot, or for List/Clear the slot asked about. */
        uint8_t     Slot() const { return query_slot_ ? query_slot_ : slot_; }
        uint8_t     RunSlot() const { return run_slot_; }
        const char *Name() const { return name_; }
        /** The upload is a new launcher, not a firmware for a key. */
        bool        ForLauncher() const { return slot_ == kLauncherSlot; }
        Status      LastStatus() const { return last_status_; }

      private:
        static constexpr uint32_t kStallMs = 3000;

        /** F0 7D 43 48 cmd ... F7, with room for the biggest DATA. */
        static constexpr size_t kHeaderLen = 5;
        static constexpr size_t kMsgMax
            = kHeaderLen + 4 + kMaxChunk + (kMaxChunk + 6) / 7 + 1;

        /* ---- interrupt side ------------------------------------------- */

        /** Called from the USB interrupt with whatever bytes arrived. Gathers
         *  one complete SysEx message into msg_ and flags it; anything that
         *  arrives while the main loop still holds the buffer is dropped, and
         *  the host's timeout resends it. */
        static void RxCallback(uint8_t *data, size_t len, void *context)
        {
            MidiUpload *self = static_cast<MidiUpload *>(context);
            for (size_t i = 0; i < len; i++)
                self->RxByte(data[i]);
        }

        void RxByte(uint8_t b)
        {
            if (b >= 0xF8)
                return; /* real-time bytes may appear anywhere; not ours */

            if (b == 0xF0)
            {
                /* Assemble straight into msg_ when it is free; otherwise
                   swallow this message whole. */
                in_sysex_ = true;
                dropping_ = msg_ready_;
                rx_len_   = 0;
            }
            if (!in_sysex_)
                return;

            if ((b & 0x80) && b != 0xF0 && b != 0xF7)
            {
                in_sysex_ = false; /* any other status ends SysEx abruptly */
                return;
            }

            if (!dropping_)
            {
                if (rx_len_ < kMsgMax)
                    msg_[rx_len_++] = b;
                else
                    dropping_ = true; /* too long to be one of ours */
            }

            if (b == 0xF7)
            {
                in_sysex_ = false;
                if (!dropping_)
                {
                    msg_len_ = rx_len_;
                    __DMB();
                    msg_ready_ = true;
                }
            }
        }

        /* ---- main loop side ------------------------------------------- */

        static uint32_t Get7(const uint8_t *p, int n)
        {
            uint32_t v = 0;
            for (int i = n - 1; i >= 0; i--)
                v = (v << 7) | (p[i] & 0x7F);
            return v;
        }

        static void Put7(uint8_t *p, uint32_t v, int n)
        {
            for (int i = 0; i < n; i++, v >>= 7)
                p[i] = v & 0x7F;
        }

        /** Whether the image carries a launcher's kLauncherTag: the prefix
         *  followed by a version number. A firmware for a key never does. */
        static bool HasLauncherTag(const uint8_t *p, uint32_t len)
        {
            static const char kPrefix[] = CHOMPI_LAUNCHER_TAG_PREFIX;
            const uint32_t    n         = sizeof(kPrefix) - 1;
            for (uint32_t i = 0; i + n < len; i++)
                if (p[i] == (uint8_t)kPrefix[0] && memcmp(p + i, kPrefix, n) == 0
                   && p[i + n] >= '0' && p[i + n] <= '9')
                    return true;
            return false;
        }

        /** A card filename the way LIST sends it: at most kMaxListName
         *  chars, anything outside printable ASCII as '?'. Returns the
         *  length; nullptr counts as "". */
        static size_t ListName(const char *name, uint8_t *out)
        {
            size_t len = 0;
            if (name)
                while (name[len] && len < kMaxListName)
                {
                    const uint8_t c = (uint8_t)name[len];
                    out[len++]      = (c >= 0x20 && c < 0x7F) ? c : '?';
                }
            return len;
        }

        static uint32_t Crc32(const uint8_t *p, uint32_t len)
        {
            uint32_t crc = 0xFFFFFFFFU;
            while (len--)
            {
                crc ^= *p++;
                for (int k = 0; k < 8; k++)
                    crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1U));
            }
            return ~crc;
        }

        /** 1-16 of A-Z 0-9 - _. It becomes part of a filename on the card,
         *  so nothing that FAT or a path could take another way. */
        static bool NameValid(const uint8_t *p, size_t len)
        {
            if (len < 1 || len > kMaxName)
                return false;
            for (size_t i = 0; i < len; i++)
            {
                const uint8_t c = p[i];
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
                      || c == '_'))
                    return false;
            }
            return true;
        }

        void Reply(uint8_t cmd, const uint8_t *payload, size_t len)
        {
            uint8_t out[64] = {0xF0, 0x7D, 0x43, 0x48, (uint8_t)(cmd | REPLY)};
            size_t  n       = kHeaderLen;
            for (size_t i = 0; i < len && n < sizeof(out) - 1; i++)
                out[n++] = payload[i];
            out[n++] = 0xF7;
            usb_.Tx(out, n);
        }

        void ReplyStatus(uint8_t cmd, Status s)
        {
            last_status_ = s;
            uint8_t p[5] = {s};
            Put7(p + 1, received_, 4);
            Reply(cmd, p, cmd == DATA ? 5 : 1);
        }

        Event Fail(uint8_t cmd, Status s)
        {
            ReplyStatus(cmd, s);
            return Event::Rejected;
        }

        Event Handle(const uint8_t *m, size_t len)
        {
            /* Not addressed to us: leave it alone, say nothing. */
            if (len < kHeaderLen + 1 || m[1] != 0x7D || m[2] != 0x43
               || m[3] != 0x48)
                return Event::None;

            const uint8_t  cmd  = m[4];
            const uint8_t *body = m + kHeaderLen;
            const size_t   blen = len - kHeaderLen - 1; /* minus F7 */

            last_heard_ = System::GetNow();
            query_slot_ = 0;

            switch (cmd)
            {
                case PING:
                {
                    uint8_t p[11] = {OK, kVersion};
                    Put7(p + 2, kMaxChunk, 4);
                    p[6] = slots_;
                    p[7] = kFeatures;
                    memcpy(p + 8, kLauncherVersion, 3);
                    Reply(cmd, p, sizeof(p));
                    return Event::Pinged;
                }

                case BEGIN:
                {
                    /* Whatever happens next, the previous upload is gone. */
                    total_    = 0;
                    received_ = 0;
                    stored_   = false;

                    if (blen < 6 || blen != 6u + body[5])
                        return Fail(cmd, BAD_MESSAGE);
                    const uint32_t total = Get7(body, 4);
                    if (total < 8 || total > capacity_)
                        return Fail(cmd, TOO_BIG);
                    const uint8_t slot = body[4];
                    if ((slot < 1 || slot > slots_) && slot != kLauncherSlot)
                        return Fail(cmd, BAD_SLOT);
                    if (!NameValid(body + 6, body[5]))
                        return Fail(cmd, BAD_NAME);

                    memcpy(name_, body + 6, body[5]);
                    name_[body[5]] = '\0';
                    slot_          = slot;
                    total_         = total;
                    ReplyStatus(cmd, OK);
                    return Event::Started;
                }

                case DATA:
                {
                    if (total_ == 0)
                        return Fail(cmd, NOT_STARTED);
                    if (blen < 4 + 2)
                        return Fail(cmd, BAD_MESSAGE);

                    const uint32_t offset = Get7(body, 4);
                    const uint8_t *in     = body + 4;
                    const size_t   in_len = blen - 4;

                    /* Decoded size: every group of up to 8 is 1 header + n. */
                    const size_t raw = in_len - (in_len + 7) / 8;
                    if (raw > kMaxChunk || offset + raw > total_)
                        return Fail(cmd, TOO_BIG);
                    if (offset > received_)
                        return Fail(cmd, BAD_OFFSET);

                    uint8_t *dst = image_ + offset;
                    for (size_t g = 0; g < in_len; g += 8)
                    {
                        const uint8_t hi = in[g];
                        for (size_t j = 1; j < 8 && g + j < in_len; j++)
                            *dst++ = in[g + j] | (((hi >> (j - 1)) & 1) << 7);
                    }

                    if (offset + raw > received_)
                        received_ = offset + raw;
                    ReplyStatus(cmd, OK);
                    return Event::Progress;
                }

                case END:
                {
                    if (total_ == 0)
                        return Fail(cmd, NOT_STARTED);
                    if (blen != 5 && blen != 6)
                        return Fail(cmd, BAD_MESSAGE);
                    end_flags_ = blen == 6 ? body[5] : 0;
                    if (received_ != total_)
                        return Fail(cmd, INCOMPLETE);
                    crc_ = Get7(body, 5);
                    if (Crc32(image_, total_) != crc_)
                        return Fail(cmd, BAD_CRC);
                    if (!ImageLooksValid(image_, total_))
                        return Fail(cmd, BAD_IMAGE);
                    if (ForLauncher() && !HasLauncherTag(image_, total_))
                        return Fail(cmd, NOT_LAUNCHER);
                    /* No reply yet: it waits until the image is on the card,
                       so the host learns the final outcome. FinishEnd(). */
                    return Event::Complete;
                }

                case LIST:
                {
                    if (blen != 1)
                        return Fail(cmd, BAD_MESSAGE);
                    if (body[0] < 1 || body[0] > slots_)
                        return Fail(cmd, BAD_SLOT);
                    /* An upload in progress keeps its image; only the card is
                       looked at or changed. */
                    query_slot_ = body[0];
                    return Event::List;
                }

                case CLEAR:
                {
                    /* slot [name_len name]: with a name, only that file goes.
                       A resent CLEAR then cannot take a file that moved onto
                       the key after the first one. */
                    if (blen != 1 && (blen < 2 || blen != 2u + body[1]
                                      || body[1] > kMaxListName))
                        return Fail(cmd, BAD_MESSAGE);
                    if (body[0] < 1 || body[0] > slots_)
                        return Fail(cmd, BAD_SLOT);
                    clear_name_given_ = blen > 1;
                    clear_name_len_   = clear_name_given_ ? body[1] : 0;
                    memcpy(clear_name_, body + 2, clear_name_len_);
                    query_slot_ = body[0];
                    return Event::Clear;
                }

                case RUN:
                {
                    if (blen != 1)
                        return Fail(cmd, BAD_MESSAGE);
                    if (body[0] < 1 || body[0] > slots_)
                        return Fail(cmd, BAD_SLOT);
                    /* Starting a slot reads it into fw_image, over whatever
                       an upload had put there. */
                    total_    = 0;
                    received_ = 0;
                    run_slot_ = body[0];
                    /* No reply yet: the caller knows whether the slot holds
                       a firmware. FinishRun(). */
                    return Event::Run;
                }

                default: return Fail(cmd, BAD_MESSAGE);
            }
        }

        MidiUsbTransport usb_;
        uint8_t         *image_    = nullptr;
        uint32_t         capacity_ = 0;
        uint8_t          slots_    = 0;

        uint32_t total_       = 0;
        uint32_t received_    = 0;
        uint32_t last_heard_  = 0;
        uint32_t crc_         = 0;
        uint8_t  slot_        = 0;
        uint8_t  query_slot_  = 0; /**< LIST/CLEAR's slot until answered */
        uint8_t  end_flags_   = 0;
        bool     stored_      = false; /**< END answered OK */
        bool     clear_name_given_ = false;
        size_t   clear_name_len_   = 0;
        uint8_t  clear_name_[kMaxListName] = {};
        uint8_t  run_slot_    = 0;
        char     name_[kMaxName + 1] = {};
        Status   last_status_ = OK;

        /* Shared with the interrupt. msg_ready_ is the handoff: the interrupt
           only writes msg_ while it is false, the main loop only reads it while
           it is true. */
        uint8_t           msg_[kMsgMax];
        volatile size_t   msg_len_   = 0;
        volatile bool     msg_ready_ = false;
        size_t            rx_len_    = 0;
        bool              in_sysex_  = false;
        bool              dropping_  = false;
    };

} // namespace chompi
