from cart import Cart
from pricing import calc_total

c = Cart()
c.add(10.0, 2)
c.add(5.0)
print("cart total:", c.total())
print("direct total:", calc_total([(1.0, 1)]))
