from pricing import calc_total

class Cart:
    def __init__(self):
        self.items = []

    def add(self, price, qty=1):
        self.items.append((price, qty))

    def total(self):
        return calc_total(self.items)
