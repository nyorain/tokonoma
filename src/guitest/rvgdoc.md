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
