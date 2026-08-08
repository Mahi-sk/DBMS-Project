"""Generates a .sql-ish command file to stress-test the B+Tree with many inserts.
Usage: python gen_stress_test.py > stress.txt
Then:  Get-Content stress.txt | .\mini_dbms.exe stress.db
"""
import sys

N = 5000  # bump this to 100000+ to really hunt for multi-level split bugs

print("CREATE TABLE items (id INT, name VARCHAR(32))")
for i in range(1, N + 1):
    print(f"INSERT INTO items VALUES ({i}, 'item{i}')")
print(f"SELECT * FROM items WHERE id = {N}")   # last row -- proves the tree still finds it
print(f"SELECT * FROM items WHERE id = 1")     # first row -- proves early data wasn't lost
print(".exit")
