TAX = 0.08

def calc_total(items):
    subtotal = sum(price * qty for price, qty in items)
    return round(subtotal * (1 + TAX), 2)
