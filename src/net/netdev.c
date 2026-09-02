/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Network device layer - one way to put a frame on the wire
*
*  The join between the network stack and whatever silicon can carry an
*  ethernet frame. netdev.h explains why the layer exists at all and why only
*  one card is ever in use; this file is the bookkeeping behind it, and it is
*  deliberately thin -- a registered driver, a counter of how many offered
*  themselves, and four accessors that forward. It is the same shape as
*  blockdev.c one subsystem over and was written by reading it.
*
*  EVERY ACCESSOR ANSWERS WITH NO CARD REGISTERED, and that is the whole
*  reason the file is worth having rather than a global function pointer.
*  A machine with no network is an ordinary machine here -- "make run" boots
*  one, and so does every laptop with the cable out -- so the shell asks these
*  questions on a machine that has nothing to answer them, several times per
*  command. If the answer were null, every one of those callers would need a
*  test in front of it, and the ones that got it wrong would fault inside a
*  printf() rather than say "none". So the answers are: send fails, mac is six
*  zero bytes, and the two strings are words. Exactly what netdev.h promises,
*  and this file is where that promise is kept.
*
*  THE OPS STRUCT IS COPIED, not referenced, for the same reason blockdev.c
*  gives: storing the pointer would make the lifetime of the driver's struct
*  this layer's problem, and a driver that ever kept its ops in something it
*  frees would leave a dangling pointer here to be found much later as a jump
*  into freed memory. Four words is nothing next to that, and the function
*  pointers inside the copy still refer to code, which is resident for as long
*  as the kernel is -- this kernel has no module unloading.
*
*  THERE IS NO netdev_unregister(). A network card is not a USB stick: it is
*  soldered to a board or screwed into a slot, and nothing in this kernel can
*  observe one leaving. Adding the call "for symmetry" would mean every
*  accessor here needs the generation dance blockdev.c does, to guard against a
*  removal that cannot happen, and net.c would need an answer to what an ARP
*  cache and a DHCP lease mean once the card they belong to is gone. When a
*  card can genuinely be pulled -- a USB ethernet adapter -- that question has
*  to be answered properly rather than in advance.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <netdev.h>

/* The one card in use, and whether there is one. The copy is only meaningful
*  while netdev_have is set; nothing here reads it otherwise. */
static netdev_ops netdev_cur;
static int        netdev_have;

/* How many drivers came to the door, taken or turned away. Counted for the
*  boot line, which is the difference between "there is no network" and "there
*  is a second card here that nothing is using" -- two states that look
*  identical from anywhere else and have completely different fixes. */
static int netdev_offers;

/* The answers for a machine with no card. netdev_zero_mac is six zero bytes
*  by virtue of being in .bss and is never written; it exists so that
*  netdev_mac() has something real to point at. A caller that formats it gets
*  "00:00:00:00:00:00", which is why the shell asks netdev_present() before
*  deciding whether to print it at all -- but a caller that forgets prints a
*  wrong MAC rather than walking a null pointer, and that is the trade this
*  was chosen for. */
static const uint8_t netdev_zero_mac[NETDEV_MAC_LEN];
static const char    netdev_no_name[]     = "none";
static const char    netdev_no_describe[] = "no network card is present";

/* --- Registration ---------------------------------------------------------
*
*  A driver arrives here once, at the end of its own bring-up, with a card it
*  has already reset, read the MAC out of and enabled. netdev.h says so in
*  words -- "a driver registers when it is up, not when it is found" -- and the
*  consequence for this file is the pleasant one: there is no "not ready yet"
*  state to represent, so every accessor below can call straight through
*  without asking a driver whether it means it.
*
*  A HALF FILLED ops STRUCT IS REFUSED, exactly as blk_register() refuses one.
*  Every member is called unconditionally further down -- netdev.h says none of
*  them may be null -- so a driver that left one out would pass registration
*  and then take the machine down on the first frame, in code whose author has
*  long since stopped looking. It is cheaper to find here.
*
*  THE SECOND CARD IS TURNED AWAY AND THAT IS NOT AN ERROR. netdev.h has the
*  argument in full: one IP address, one ARP cache, one default gateway and one
*  lease mean there is no honest answer to which of two cards an outgoing frame
*  belongs to. The incumbent stays rather than the newcomer, because the
*  incumbent is the one net_init() may already have taken a MAC address from.
*  What the refused driver does about it is its own business -- rtl8139.c shuts
*  its card back down, which is the right answer for a card that would
*  otherwise sit there DMAing frames into a ring nobody drains. */
int netdev_register(const netdev_ops *ops)
{
    if(ops == 0)
    {
        /* Not counted as an offer. The counter exists to say that a card was
        *  found, and a null pointer is a bug in a caller rather than a card. */
        return -1;
    }

    netdev_offers++;

    if(ops->name == 0 || ops->send == 0 || ops->mac == 0 || ops->describe == 0)
    {
        return -1;
    }

    if(netdev_have)
    {
        return -1;
    }

    netdev_cur  = *ops;         /* copied, see the file header               */
    netdev_have = 1;
    return 0;
}

int netdev_present(void)
{
    return netdev_have;
}

int netdev_offered(void)
{
    return netdev_offers;
}

/* --- What the stack and the shell ask for --------------------------------- */

/* No card is a refusal rather than a fault, because the stack calls this from
*  net_send() on every frame and a machine with no card is a machine that
*  simply never gets one out. net_up() is false there anyway, so in practice
*  this is reached only by a caller that skipped the check -- and a negative
*  return is what that caller was going to get from a real card that would not
*  take the frame either.
*
*  The two argument checks are here rather than in each driver, for the reason
*  blockdev.c states about its own: a driver is busy programming hardware and
*  every argument it has to distrust is one more thing to get wrong in the
*  code that is already hardest to read. A driver below this line may assume a
*  non null frame and a non zero length. The upper bound is deliberately NOT
*  checked here -- what a card can carry is the card's own number, and netdev.h
*  makes refusing an over-long frame part of send()'s contract. */
int netdev_send(const void *frame, uint32_t len)
{
    if(!netdev_have)
    {
        return -1;
    }
    if(frame == 0 || len == 0)
    {
        return -1;
    }
    return netdev_cur.send(frame, len);
}

/* Six bytes, always, and never null -- see the file header for why that
*  matters more than it looks like it should.
*
*  A driver that answers null anyway gets the zero address rather than passing
*  the null on. netdev.h says a driver must never do that and a driver that
*  could not read its own MAC has no business registering; this is the belt to
*  that braces, and it costs one comparison on a path that runs once per boot
*  in net_init() and once per "ifconfig". */
const uint8_t *netdev_mac(void)
{
    const uint8_t *mac;

    if(!netdev_have)
    {
        return netdev_zero_mac;
    }

    mac = netdev_cur.mac();
    if(mac == 0)
    {
        return netdev_zero_mac;
    }
    return mac;
}

/* The chip's name, short enough for a boot line and for a column of
*  "ifconfig". "none" is a real answer here and reads correctly in both of the
*  sentences the shell builds out of it. */
const char *netdev_name(void)
{
    if(!netdev_have || netdev_cur.name == 0)
    {
        return netdev_no_name;
    }
    return netdev_cur.name;
}

/* The long form: whatever the driver thinks is worth knowing when the network
*  does not work. Asked once per "ifconfig", so a driver is free to build it
*  by hand into a static buffer, which is what both drivers do -- there is no
*  snprintf() in this kernel. */
const char *netdev_describe(void)
{
    const char *text;

    if(!netdev_have)
    {
        return netdev_no_describe;
    }

    text = netdev_cur.describe();
    if(text == 0)
    {
        return netdev_no_describe;
    }
    return text;
}
