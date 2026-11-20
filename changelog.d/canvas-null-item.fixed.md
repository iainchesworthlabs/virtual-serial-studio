**The GPU canvas skips an item that is not there instead of reading through a null pointer.** No crash was seen; the check was added because code analysis could not rule one out.
