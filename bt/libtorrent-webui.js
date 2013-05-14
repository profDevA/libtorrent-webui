read_string = function(view, offset)
{
	var len = view.getUint16(offset);
	var str = '';
	offset += 2;
	for (var j = 0; j < len; ++j)
	{
		str += String.fromCharCode(view.getUint8(offset));
		offset += 1;
	}
	return str;
}

read_uint64 = function(view, offset)
{
	var high = view.getUint32(offset);
	offset += 4;
	var low = view.getUint32(offset);
	offset += 4;
	return high * 4294967295 + low;
}

libtorrent_connection = function(url, callback)
{
	var self = this;

	this._socket = new WebSocket(url);
	this._socket.onopen = function(ev) { callback("OK"); };
	this._socket.onerror = function(ev) { callback(ev.data); };
	this._socket.onmessage = function(ev)
	{
		var view = new DataView(ev.data);
		var fun = view.getUint8(0);
		var tid = view.getUint16(1);

		if (fun >= 128)
		{
			var e = view.getUint8(3);
			fun &= 0x7f;
			console.log('RESPONSE: fun: ' + fun + ' tid: ' + tid + ' error: ' + e);

			if (!self._transactions.hasOwnProperty(tid)) return;

			var handler = self._transactions[tid];
			delete self._transactions[tid];

			// this handler will deal with parsing out the remaining
			// return value and pass it on to the user supplied
			// callback function
			handler(view, fun, e);
		}
		else
		{
			// This is a function call

		}

	};
	this._socket.binaryType = "arraybuffer";
	this._frame = 0;
	this._transactions = {}
	this._tid = 0;

	this.get_updates = function(callback)
	{
		if (this._socket.readyState != WebSocket.OPEN)
		{
			window.setTimeout( function() { callback(null); }, 0);
			return;
		}
		
		var tid = self._tid++;
		if (self._tid > 65535) this._tid = 0;

		// this is the handler of the response for this call. It first
		// parses out the return value, the passes it on to the user
		// supplied callback.
		this._transactions[tid] = function(view, fun, e)
		{
			self._frame = view.getUint32(4);
			var num_torrents = view.getUint32(8);
			console.log('frame: ' + self._frame + ' num-torrents: ' + num_torrents);
			ret = {};
			var offset = 12;
			for (var i = 0; i < num_torrents; ++i)
			{
				var infohash = '';
				for (var j = 0; j < 20; ++j)
				{
					var b = view.getUint8(offset + j);
					infohash += b.toString(16);
				}
				offset += 20;
				var torrent = {};

				var mask_high = view.getUint32(offset);
				offset += 4;
				var mask_low = view.getUint32(offset);
				offset += 4;

				for (var field = 0; field < 32; ++field)
				{
					var mask = 1 << field;
					if ((mask_low & mask) == 0) continue;
					switch (field)
					{
						case 0: // flags
							// skip high bytes, since we can't
							// represent 64 bits in one field anyway
							offset += 4;
							torrent['flags'] = view.getUint32(offset);
							offset += 4;
							break;
						case 1: // name
							var name = read_string(view, offset);
							offset += 2 + name.length;
							torrent['name'] = name;
							break;
						case 2: // total-uploaded
							torrent['total-uploaded'] = read_uint64(view, offset);
							offset += 8;
							break;
						case 3: // total-downloaded
							torrent['total-downloaded'] = read_uint64(view, offset);
							offset += 8;
							break;
						case 4: // added-time
							torrent['added-time'] = read_uint64(view, offset);
							offset += 8;
							break;
						case 5: // completed-time
							torrent['completed-time'] = read_uint64(view, offset);
							offset += 8;
							break;
						case 6: // upload-rate
							torrent['upload-rate'] = view.getUint32(offset);
							offset += 4;
							break;
						case 7: // download-rate
							torrent['download-rate'] = view.getUint32(offset);
							offset += 4;
							break;
						case 8: // progress
							torrent['progress'] = view.getUint32(offset);
							offset += 4;
							break;
						case 9: // error
							var e = read_string(view, offset);
							offset += 2 + e.length;
							torrent['error'] = e;
							break;
						case 10: // connected-peers
							torrent['connected-peers'] = view.getUint32(offset);
							offset += 4;
							break;
						case 11: // connected-seeds
							torrent['connected-seeds'] = view.getUint32(offset);
							offset += 4;
							break;
						case 12: // downloaded-pieces
							torrent['downloaded-pieces'] = view.getUint32(offset);
							offset += 4;
							break;
						case 13: // total-done
							torrent['total-done'] = read_uint64(view, offset);
							offset += 8;
							break;
						case 14: // distributed-copies
							var integer = view.getUint32(offset);
							offset += 4;
							var fraction = view.getUint32(offset);
							offset += 4;
							torrent['distributed-copies'] = integer + (fraction / 1000.0);
							break;
						case 15: // all-time-upload
							torrent['all-time-upload'] = read_uint64(view, offset);
							offset += 8;
							break;
						case 16: // all-time-download
							torrent['all-time-download'] = read_uint64(view, offset);
							offset += 8;
							break;
						case 17: // unchoked-peers
							torrent['unchoked-peers'] = view.getUint32(offset);
							offset += 4;
							break;
						case 18: // num-connections
							torrent['num-connections'] = view.getUint32(offset);
							offset += 4;
							break;
						case 19: // queue-position
							torrent['queue-position'] = view.getUint32(offset);
							offset += 4;
							break;
						case 20: // state
							torrent['state'] = view.getUint8(offset);
							offset += 1;
							break;
						case 21: // failed-bytes
							torrent['failed-bytes'] = read_uint64(view, offset);
							offset += 8;
							break;
						case 22: // redundant-bytes
							torrent['redundant-bytes'] = read_uint64(view, offset);
							offset += 8;
							break;
					}
				}
				ret[infohash] = torrent;
			}
			callback(ret);
		}

		var call = new ArrayBuffer(15);
		var view = new DataView(call);
		// function 0
		view.setUint8(0, 0);
		// transaction-id
		view.setUint16(1, tid);
		// frame-number
		view.setUint32(3, this._frame);
		// TODO: mask should be passed in!
		// bitmask (64 bits) [flags, name, progress, connected-peers, state, error, upload-rate, download-rate]
		var mask = (1 << 0) | (1 << 1) | (1 << 8) | (1 << 10) | (1 << 20) | (1 << 9) | (1 << 6) | (1 << 7);
		view.setUint32(7, 0);
		view.setUint32(11, mask);

		console.log('CALL get_updates( frame: ' + this._frame + ' mask: ' + mask.toString(16) + ' ) tid = ' + tid);
		this._socket.send(call);
	}
}

