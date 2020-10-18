The `updateDevice` method of the Pool classes returns whether gpu
bindings have changed. If they return true, all used descriptors 
(`rvg::DrawDescreiptor`) using them have to be updated. Those
in turn will return whether a re-record of the command buffer is needed.
So your typical upload function should look like this (you can
obviously optimize further internally and only call `updateDevice`
on the pools that might have changed):

```
bool rebind = false;
bool rerec = false;

// for every active pool
rebind |= pool.updateDevice();
...

// exception: DrawPool
bool drawPoolChanged = drawPool.updateDevice();
rebind |= drawPoolChanged;
rerec |= drawPoolChanged;

if(rebind) {

	// for every active descriptor using any of those pools
	rerec |= ds.updateDevice()
	...

	if(rerec) {
		recordCommandBuffers();
	}
}
```

Exceptions already seen above: `rvg::DrawPool`.
If that pool returns true from `updateDevice`, you will always
need a rerecord.

...nevermind, same for vertex and index pools...

---

The coordinates given to rvg will be multiplied with the bound transform
(identity transform by default) and are then expected in range from
0 to the current viewport size. Why is it done like this instead of
the old way, expecting them in normalized coordinates already (and requiring
users that want to define polygons in window coordinates to simply
use a transform)?

Because certain operations inside rvg (like the pixel-perfect edge-antialiasing 
in rvg::Polygon) have to know the size of one pixel. And, more importantly,
they become a whole lot easier when the coordinate system the operations
are done in is not scaled, i.e. pixel width is the same as pixel height
(this would not be the case for normalized device coordinates).

This also serves as a reminder: using rvg::Transform to scale content (including
skewing) may break all those things that require to know pixel size, like
anti aliasing, curve baking (e.g. the rectangle roundings) or text.
All scaling must be done *before* actual polygons are baked. For rvg::Polygon,
this means you simply multiply your transform will all points, before
passing them to the polygon. The high-level polygon utility classes
like rvg::CircleShape and rvg::RectShape have preTransform properties.
So does rvg::Text. This also means that changing the scaling of such
objects every frame requires potentially a lot of re-baking and re-uploading.
A lot of other toolkits re-bake and re-upload *everything*, *every frame*
so for most usecases this should not be a problem. Just something to keep
in mind when optimizing and thinking about large-scale usecases.

---

sketch #1:

At the end of update(), call update() on all buffers. It's important though
that they are not changed after that until updateDevice(). If any
of the update functions returns true, a buffer was changed and so the
draw descriptor has to be updated (without descriptor indexing, that
means that also a re-record is needed but this will automatically
correctly be returned by updateDevice() on the updated descriptor).

Alternative: just put all that update check logic in updateDevice for now?
mean we do all the recording in updateDevice() but that shouldn't be
such a huge problem I guess (for now at least).

So, when any buffer returned true from updateDevice (or anything
in your scene changed), just re-batch the whole scene (with rvg, see draw.hpp). 
Then (should be done in every frame but if DrawInstance's aren't used to manually manipulate
a recorded DrawCall or manually change DrawDescriptors it's ok to only do it
after re-batching) call updateDevice on all draw calls and draw descriptors.
If any of those return true, you need to rerecord the vulkan command buffer.
This happens if:
- a descriptor buffer (clip/transform/paint/cmdBuf) is recreated and there is no
  descriptor indexing
- textures of a DrawDescriptor are changed and there is no descriptor indexing
- a vertex or index buffer changes
- a new draw descriptor or draw call is created
- the number of draws in a draw call changes

---

easy-to-use api, old style:

1. create a rvg context
2. create all your primitives and paints (directly from the context)
3. use directly the DrawRecorder from rvg, the default scene
4. when rvg updateDevice returns rerecord; rerecord!

Basically: rvg::Context has a default of everything: scene and pools etc
	that can be used for simple drawing. Automatically managed multiple
	DrawPools for all the required texture slots.

could make it even closer to rvg 0.1 style by combining the draw batching
and command buffer recording into one step? Like

```
rvg::DrawBufRecorder rec = ctx.record(cb);
// ... do stuff with rec, paint and shit ...

// when rec goes out of scope, the commands are recorded to the command buffer
```

have to evaluate whether this has a big performance impact though. I think
there might be some cases where only rebatching would be needed and not actual
rerecording.

---

on the other hand, the new lowest level api:

- create your pools on your own
- manage scenes, only rebatch scenes that need it
	- also: can record different scenes to different command buffers

---

initialization:

- create pool objects (no gpu buffers yet)
- allocate stuff from pools (only in logical state, still no gpu buffers)
- updateDevice on pools

- (if needed) update DrawCalls & DrawDescriptors
	- this will possibly increase the needed size of the DrawPool
- updateDevice on DrawPool
- (if needed) record command buffers
