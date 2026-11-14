# общая случайность повреждений самостоятельных конструкций
import math
import random


class VariedDamage:
    def __init__(self,base,amount,seed):
        self.base=base
        self.amount=amount/100.0
        self.scale=max(min((part[2] for part in base.parts),default=1.0),1e-6)
        rng=random.Random(seed)
        self.waves=[(tuple(rng.uniform(-1.3,1.3) for _ in range(3)),rng.uniform(0,2*math.pi)) for _ in range(3)]

    def ratio(self,point):
        value=self.base.ratio(point)
        if not self.amount or not math.isfinite(value):return value
        noise=sum(math.sin(sum(point[i]*axis[i] for i in range(3))*math.pi/self.scale+phase)
            for axis,phase in self.waves)/3.0
        return value*(1.0+self.amount*.8*(.5+.5*noise))
