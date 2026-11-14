# храним готовые результаты в ограниченном по размеру кэше
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded plain-data memoization, no RNA and no shared mutable return values."""
from collections import OrderedDict
import hashlib,pickle

class Results:
    def __init__(self,limit=64*1024*1024):self.limit=limit;self.bytes=0;self.values=OrderedDict();self.hits=0
    def clear(self):self.values.clear();self.bytes=0
    def key(self,module,name,args,kwargs):
        # Large arrays are hashed in place (out-of-band buffers) instead of being copied into the pickle.
        try:
            buffers=[];h=hashlib.blake2b(pickle.dumps((module,name,args,kwargs),protocol=5,buffer_callback=buffers.append),digest_size=16)
            for buffer in buffers:
                with buffer.raw() as view:h.update(len(view).to_bytes(8,'little'));h.update(view)
            return h.digest()
        except (TypeError,ValueError,BufferError,pickle.PickleError):return None
    def get(self,key):
        if key is None or key not in self.values:return False,None
        self.hits+=1;blob=self.values.pop(key);self.values[key]=blob
        return True,pickle.loads(blob)
    def put(self,key,value):
        if key is None:return
        try:blob=pickle.dumps(value,protocol=5)
        except (TypeError,pickle.PickleError):return
        if len(blob)>self.limit:return
        previous=self.values.pop(key,None)
        if previous is not None:self.bytes-=len(previous)
        while self.values and (self.bytes+len(blob)>self.limit or len(self.values)>=32):self.bytes-=len(self.values.popitem(last=False)[1])
        self.values[key]=blob;self.bytes+=len(blob)
