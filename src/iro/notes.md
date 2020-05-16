Network notes & ideas:
----------------------

In theory, we can make the windowed-lockstep algorithm work even for
high pings and slow network bandwidths. To make it work for high ping
times we simply increase the delay from input to action performed.
When the ping is 1000ms (pretty high, most games are not playable
at all at this point) we simply make sure that the delay is a second
or two (technically less than a second should be enough but given that
there may be packet loss and some fluctuation of travel times it's
a safer bet to take once or twice the ping time I guess).
After giving a command, it will then take a second or two until the
command is performed. That's very ugly but it should always be possible
to simply cover it up visually, selling it as "build-up times" or using
simple markers that immediately show up after the input was performed
and signal that the operation is pending.

Limited bandwidths shouldn't be a problem in general since we don't
send large packets. **But** we send one packet per step, even if it does
not contain any input. Given a 60 Hz step-rate that's 60 packets per
second. Normally that should not be any problem at all. But still,
we could dynamically downgrade to a 30 Hz step-rate (obviously increasing
the fixed simulation time-delta so that the perceived simulation speed
stays the same). Probably worth to play around with even higher step-rates,
like 100, 120 or 144 Hz.

A problem regarding step-rates: what happens if one client just can't
deliver this computation performance? The maps and simulations will get
more complicated (and bigger!) over time, we should probably build
some **synchronization** into the protocol already.
At the moment, the game will automatically slow down but that means
any client could basically implicitly stop the game if they
feel like it, I don't like that. 
If a client can't deliver the minimum requirements, that should be
detected and the game end. I guess defining the minimum
requirements as a 30 Hz step rate seems fine (but on the other
hand that seems kinda unfair to the players that can easily do
60 or >100Hz and are implicitly downgraded). Easier than any
dynamic adjustment would be this: Just fix the step-rate before
a game starts (how this is done can be changed, either let the
players decide or do it automatically based on previous experiences
or dummy tests) and when a client can't keep up over long times,
eventually just end the game.

The last confirmation that actually starts a game (or later: the
message that confirms both clients are in the game) contains a time point
(given in a platform independent way) that marks t-0. The simulation starts
e.g. 5 seconds after t-0. At that point, clients should present the
first step and send out the message containing the input for
step 0. The further steps are defined to take place at the stepping
intervals. When a client slows down temporarily it's their job
to catch up. When they stay behind for too long (probably best to
let the other client decide, could also just be a shitty connection
but anyways:) just end the game (or show a message or whatever;
we have detected an issue in this case).

---

Possible optimization:
Don't send a packet every step if there is no input, only every couple
of frames. But this increases the delay we need.

Possible different approach:
Stray away from the strict lock-stepping. At the moment, clients will
*never* show any content that isn't the truth (compared to other games,
where game state is interpolated/extrapolated locally leading
to issues such as rubber-banding and glitching). When we simply
assume that the other client did not perform any actions even if we have
not received their packet yet (on reliable connections we can be fairly
certain at some point that there was no input for the given step from
the other side if we don't receive a packet) and can therefore step
ahead. When we then receive a input packet later on (e.g. by detecting
it was lost by comparing sequence numbers or something) we just re-simulate
from the step at which we made this wrong assumption.
This may be needed to fix problems in future but unless we just can't sanely
work around this kind of approach anymore, let's keep this out of the protocol,
showing the truth and nothing but the truth.

---

float-math in compute shaders:

We can probably use (approximations) of sqrt, exp and trig functions
by simply approximating them via textures. This obviously
only works when they have limited range.
We could also use a custom implementation for fast inverse square root,
then we could normalize vectors again.

