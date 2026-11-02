# The wallet, simulated: what the payment conversation is, and what it is not

This file describes the contactless-payment path that `devices/b651/drivers/nfc/hakios.nfc.c` models and
`devices/b651/tests/wallet_test.c` exercises. **It is a simulation of an APDU conversation and nothing else.** There
is no card, no token, no key, no attestation, and no money anywhere in it.

Read this file before reading any of the code as a wallet.

## What is simulated

One thing: the three APDUs a contactless terminal sends a payment card at the start of a transaction, and the exact
bytes the modeled chip answers them with. Nothing above or below that is simulated.

```text
   terminal (wallet_test.c)                      modeled ST54 (/dev/hakios-nfc, nfc=1 nfc_model=1)
        |  ioctl POWER(1), USE_ESE(1), CARD(1)  |   <- power sequencing, eSE select (the real driver code)
        |                                       |
        |  00 A4 04 00 0E "2PAY.SYS.DDF01" 00   |
        | ------------------------------------> |   SELECT PPSE
        | <------------------------------------ |   6F FCI: one application, the Visa AID      90 00
        |                                       |
        |  00 A4 04 00 07 A0 00 00 00 03 10 10  |
        | ------------------------------------> |   SELECT AID
        | <------------------------------------ |   6F FCI with a 9F38 PDOL                    90 00
        |                                       |
        |  80 A8 00 00 3B 83 39 <57 bytes> 00   |
        | ------------------------------------> |   GET PROCESSING OPTIONS (PDOL data filled)
        | <------------------------------------ |   77 AIP + AFL                                90 00
```

That is the whole of it. A real transaction continues past this point and the simulation does not.

| APDU | request (bytes) | response (bytes) | status |
|---|---|---|---|
| SELECT PPSE | `00 A4 04 00 0E 32 50 41 59 2E 53 59 53 2E 44 44 46 30 31 00` | 52-byte FCI: `6F 30` / `84 0E 32 50 41 59 2E 53 59 53 2E 44 44 46 30 31` / `A5 1E BF 0C 1B 61 19` / `4F 07 A0 00 00 00 03 10 10` / `50 0B "VISA CREDIT"` / `87 01 01` / `90 00` | `90 00` |
| SELECT AID | `00 A4 04 00 07 A0 00 00 00 03 10 10 00` | 96-byte FCI: `6F 5C` / `84 07 A0 00 00 00 03 10 10` / `A5 51` / `50 0B "VISA CREDIT"` / `87 01 01` / `5F 2D 02 65 6E` / `9F 38 39` + 57 bytes of PDOL tag/length pairs / `90 00` | `90 00` |
| GET PROCESSING OPTIONS | `80 A8 00 00 3B 83 39 <PDOL data> 00` | 14 bytes: `77 0A` / `82 02 00 00` (AIP: no auth method claimed) / `94 04 08 01 01 00` (AFL: SFI 1, record 1) / `90 00` | `90 00` |

The TLV is correct in form: every tag and length in those arrays parses, and `wallet_test.c` compares the replies
byte for byte rather than accepting a header. What is inside them is invented:

- the application label is a fixed string, not a card's;
- the AIP claims **no** authentication method (SDA, DDA, CDA are all clear). A real card sets one, because a real
  card can prove it holds a key. This one cannot, so claiming otherwise would be a lie in the first byte a terminal
  reads;
- the AFL points at SFI 1 record 1, which does not exist. `READ RECORD` is not modeled, so a terminal that follows
  the AFL gets `6A 82` and stops;
- the PDOL data (amount, date, unpredictable number) is all zero. The model checks the *shape* of that data and
  ignores it.

The card's presence is a control, `HAKIOS_NFC_CARD` (`_IOW('N', 5, uint32_t)`), and it exists **only in the model**.
A real ST54 has no such register: a card is either in the field or the terminal hears nothing. On a non-modeled
chip the ioctl is refused with `ENOTSUP` instead of silently succeeding, so a caller cannot mistake a stub for
hardware. With no card, every `SELECT` and `GPO` answers `6A 82` -- a defined answer, so a host driving the device
fails instead of hanging. A reset or a power cycle also drops the selected application, so `GPO` before a `SELECT`
answers `69 85`.

## What is NOT simulated

Everything else. Specifically, and in the order a real wallet would need it:

| a real wallet needs | here |
|---|---|
| token provisioning: the issuer's TSP pushes a token, the eSE stores it | ✗ nothing provisions anything. The "card" is three constant arrays in a driver |
| attestation: the phone proves to the TSP it is a genuine device (`Play Integrity` / hardware key) | ✗ not possible, and not attempted. See `NFC.md` and `MiniAppleKeyStore.cpp`: its "wrapping" is reversible and protects nothing |
| a real card key and a cryptogram: `GENERATE AC` signs the transaction data | ✗ impossible. The model holds no key; `GENERATE AC` is not answered |
| cardholder verification: PIN, biometric, CDCVM | ✗ nothing |
| terminal risk management, the terminal's own keys, acquirer/issuer host messages | ✗ nothing. There is no terminal here, only a test program sending bytes |
| a payment network, an amount, a merchant, money | ✗ nothing. The amounts in the PDOL data are zero and no amount is ever asserted |
| the ST54's real transport: SPI framing, the field, the antenna | ✗ nothing. `NFC.md` says why the framing is not in the vendor source either |

`GENERATE AC` is the line. A real transaction cannot complete without a cryptogram, and a cryptogram needs a key the
eSE holds and that nothing in this tree can produce, provision, or impersonate.

## The gap to a real wallet on this phone

The phone has the hardware: an ST54 with an eSE, and an Android stack that on the vendor image talks to it over SPI.
That hardware *can* emulate a contactless card -- that is what an eSE is for. But a working mobile wallet on it needs
four things this port does not have, in increasing order of difficulty:

1. **The NFC driver and its pins.** Which of `spi0..spi7`, the chip select, the IRQ and the reset GPIO. A `dtbo`
   read or a logic analyser, not a code question (`NFC.md`).
2. **A host-side stack**: the `ST21NFC_*` ioctl ABI and an APDU router. `hakios.nfc.c` is the first part of this
   and the payment path above is the router's shape, but a real router must talk to a real eSE, not to constants.
3. **Provisioning.** A token has to be issued by the card network's TSP, over an authenticated channel, by a
   device that can prove what it is. Nothing in this port can do step one of that.
4. **Attestation.** Google Wallet in particular requires Play Services and Play Integrity, which needs a hardware
   key this kernel does not have. `NFC.md` already draws the line: Google Wallet is not on the table, and no NFC
   driver changes that.

So the honest summary: **the conversation works, end to end, in simulation. The wallet does not exist.** Steps 1 and
2 are portable work this tree can do. Steps 3 and 4 need credentials, a network, and hardware attestation that
this port does not have and cannot fake honestly.

## Rules

Same as `NFC.md`: the bootloader is locked and nothing here writes to it. Nothing in this simulation contacts a
network, stores a credential, or claims to. Do not put a real card number, token, or key into these files; there is
no field in the model that would do anything but ignore it.
