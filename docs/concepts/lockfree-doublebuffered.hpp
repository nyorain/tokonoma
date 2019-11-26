
template <typename T>
class DoubleBuffered {
public:
	// - Producer thread -
	bool enque(T& val) {
		return update_.enqueue(val);
	}

	bool peek(T& val) {
		// In this case we are not allowed to read current_ since
		// it may be updated any time
		// TODO: when T is trivial, we could simply read update_.value in
		// this case. But that doesn't work if the render thread moves
		// from update_.value at the same time.
		if(update_.readable()) {
			return false;
		}

		val = current_;
		return true;
	}

	// Returns whether the value was updated
	bool set(T& val) {
		// In this case we are not allowed to read current_ since
		// it may be updated any time
		if(update_.readable()) {
			return false;
		}

		// we are still not allowed to write current_.
		// we never are
		if(val != current_) {
			update_.enqueue(val);
			return true;
		}

		return false;
	}

	bool writable() {
		return update_.writable();
	}

	// - Consumer thread -
	bool update() {
		return update_.dequeue(current_);
	}

	const T& get() const {
		return current_;
	}

	const T& read() const {
		update();
		return get();
	}

private:
	Shared<T> update_;
	T current_;
};

