- default behavior: 
  we assume that App::update isn't too expensive and can be called
  with *any* frequency
alternatively:
- Apps can signal that update is expensive and should only be
  called when App::scheduleUpdate was requested (in an event)
- Apps can also pass a time point to scheduleUpdate, meaning
  that the next update should be done at (as soon as possible after)
  the given time point

In general, apps can get multiple update behaviors this way:
- just update on every input event (checking e.g. if the internal state
  changes require a redraw/rerecord/whatever)
- update in spare CPU time, even when no redraw is ever necessary.
  Either as often as possible (always calling scheduleUpdate without
  time parameter, this is a bad idea usually though since then
  nothing in the main loop blocks anymore) or with a fixed maximum
  frequency (e.g. update no more than 30 times per second since that
  is needed for a physics system or something) by scheduling updates
  at the respective time points.


