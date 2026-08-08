"""
Labeled training examples for the intent classifier.

Each entry is (natural_language_text, intent_label).
Intents map directly to statement types mini-dbms's SQL parser supports:
    CREATE_TABLE, DROP_TABLE, SHOW_TABLES, INSERT, SELECT, UPDATE, DELETE,
    RENAME_COLUMN

This is intentionally a small, hand-written dataset -- enough to train a
simple bag-of-words classifier that generalizes across phrasing variety.
In a production system you'd collect real user queries and retrain
periodically, or fall back to an LLM for anything outside this distribution.
"""

EXAMPLES = [
    # --- CREATE_TABLE ---
    ("create a table called users with columns id int, name varchar 32, email varchar 64", "CREATE_TABLE"),
    ("make a new table named products with id as int and title as varchar 50", "CREATE_TABLE"),
    ("i want to create a table students with id int name varchar 40", "CREATE_TABLE"),
    ("set up a table called orders with columns id int and status varchar 20", "CREATE_TABLE"),
    ("create table employees id int name varchar 30 department varchar 20", "CREATE_TABLE"),
    ("build a new table for books with id int and title varchar 100", "CREATE_TABLE"),
    ("define a table called accounts with id int balance int", "CREATE_TABLE"),
    ("i need a table customers with id int name varchar 32", "CREATE_TABLE"),
    ("start a new table inventory with id int item varchar 40 quantity int", "CREATE_TABLE"),
    ("create table users having id int and name varchar 32", "CREATE_TABLE"),
    ("make table teachers with columns id int subject varchar 30", "CREATE_TABLE"),
    ("please create a new table logs with id int message varchar 100", "CREATE_TABLE"),

    # --- INSERT ---
    ("add a user with id 1 name alice and email alice@example.com to users", "INSERT"),
    ("insert into users id 2 name bob email bob@example.com", "INSERT"),
    ("add a new row to products with id 5 title laptop", "INSERT"),
    ("insert a record into students id 10 name john", "INSERT"),
    ("put a new entry in orders with id 1 status pending", "INSERT"),
    ("add id 3 name charlie email charlie@mail.com into users table", "INSERT"),
    ("create a new row for employees id 4 name diana department sales", "INSERT"),
    ("insert id 7 title dune into books", "INSERT"),
    ("add a customer with id 9 name eve to customers", "INSERT"),
    ("store a new item id 2 item pen quantity 100 in inventory", "INSERT"),
    ("insert into accounts id 1 balance 500", "INSERT"),
    ("add a log entry id 1 message system started to logs", "INSERT"),
    ("add in pets the dog , 1 , bhow", "INSERT"),
    ("insert in pets the row with dog 1 bhow", "INSERT"),
    ("add in users the alice , 1 , alice@mail.com", "INSERT"),
    ("insert in students the row with john 10 physics", "INSERT"),
    ("add to orders the row 1 pending", "INSERT"),
    ("put in accounts a row with 500 1", "INSERT"),
    ("insert row into books dune 7", "INSERT"),
    ("add new pets entry cat 2 meow", "INSERT"),
    ("insert a new record dog 3 woof into pets", "INSERT"),

    # --- SELECT ---
    ("show me all users", "SELECT"),
    ("show all rows from products", "SELECT"),
    ("get all students", "SELECT"),
    ("list everything in orders", "SELECT"),
    ("show the user with id 1", "SELECT"),
    ("find the product where id equals 5", "SELECT"),
    ("get me the student with id 10", "SELECT"),
    ("what is in customers table", "SELECT"),
    ("display all employees", "SELECT"),
    ("show me the book with id 7", "SELECT"),
    ("find user where id is 3", "SELECT"),
    ("fetch the order with id 1", "SELECT"),
    ("show all accounts", "SELECT"),
    ("get the account with id 1", "SELECT"),
    ("list all logs", "SELECT"),
    ("show entry with id 2 from inventory", "SELECT"),

    # --- DROP_TABLE ---
    ("delete the pets table", "DROP_TABLE"),
    ("drop the users table", "DROP_TABLE"),
    ("remove the products table", "DROP_TABLE"),
    ("get rid of the orders table", "DROP_TABLE"),
    ("delete table students", "DROP_TABLE"),
    ("drop table accounts", "DROP_TABLE"),
    ("please delete the logs table", "DROP_TABLE"),
    ("i want to remove the customers table", "DROP_TABLE"),
    ("can you drop the inventory table", "DROP_TABLE"),
    ("delete the whole books table", "DROP_TABLE"),
    ("get rid of table employees", "DROP_TABLE"),
    ("drop the entire teachers table", "DROP_TABLE"),
    ("erase the pets table", "DROP_TABLE"),
    ("destroy the orders table", "DROP_TABLE"),
    ("delete users table for me", "DROP_TABLE"),
    ("drop pets", "DROP_TABLE"),
    ("drop table pets", "DROP_TABLE"),
    ("drop accounts", "DROP_TABLE"),
    ("i dont need the orders table anymore delete it", "DROP_TABLE"),
    ("get rid of pets", "DROP_TABLE"),
    ("remove students", "DROP_TABLE"),

    # --- DELETE (row-level, not the whole table -- always mentions a row/
    # entry/record or a WHERE-ish condition, never bare "table") ---
    ("delete the row from users where id is 1", "DELETE"),
    ("delete from pets where id equals 5", "DELETE"),
    ("remove the row with id 3 from orders", "DELETE"),
    ("delete the entry where name is alice from users", "DELETE"),
    ("remove all rows from logs", "DELETE"),
    ("delete every row in pets", "DELETE"),
    ("delete the record where id is 10 from students", "DELETE"),
    ("remove the pet whose name is fluffy", "DELETE"),
    ("delete the user with id 7", "DELETE"),
    ("get rid of the row where status is pending in orders", "DELETE"),
    ("remove entry id 2 from inventory", "DELETE"),
    ("delete all entries from accounts", "DELETE"),
    ("clear all rows from employees", "DELETE"),
    ("delete the account where balance is 0", "DELETE"),
    ("remove the book where title is dune", "DELETE"),
    ("delete row id 9 from customers", "DELETE"),
    ("delete everything from logs", "DELETE"),
    ("remove the order with id 4", "DELETE"),

    # --- UPDATE ---
    ("update users set name to bob where id is 1", "UPDATE"),
    ("change the status to shipped in orders where id is 1", "UPDATE"),
    ("update the age of the user with id 2 to 30", "UPDATE"),
    ("set balance to 500 in accounts where id is 1", "UPDATE"),
    ("update pets set name to rex where id is 3", "UPDATE"),
    ("change the title to hobbit in books where id is 7", "UPDATE"),
    ("update the department of employee id 4 to sales", "UPDATE"),
    ("set quantity to 50 in inventory where id is 2", "UPDATE"),
    ("update students set name to john where id is 10", "UPDATE"),
    ("change the email to alice@new.com in users where id is 1", "UPDATE"),
    ("update order id 1 set status to delivered", "UPDATE"),
    ("set the sound to meow in pets where id is 2", "UPDATE"),

    # --- RENAME_COLUMN ---
    ("rename column age to years_old in users", "RENAME_COLUMN"),
    ("rename the name column in pets to nickname", "RENAME_COLUMN"),
    ("change column title to book_title in books", "RENAME_COLUMN"),
    ("rename status to state in orders", "RENAME_COLUMN"),
    ("rename column email to contact_email in users", "RENAME_COLUMN"),
    ("in pets rename column sound to noise", "RENAME_COLUMN"),
    ("change the department column to dept in employees", "RENAME_COLUMN"),
    ("rename balance to amount in accounts", "RENAME_COLUMN"),
    ("rename column quantity to qty in inventory", "RENAME_COLUMN"),

    # --- SHOW_TABLES ---
    ("show tables", "SHOW_TABLES"),
    ("list all tables", "SHOW_TABLES"),
    ("what tables do i have", "SHOW_TABLES"),
    ("show me all the tables", "SHOW_TABLES"),
    ("list my tables", "SHOW_TABLES"),
    ("what tables exist", "SHOW_TABLES"),
    ("show all tables in this database", "SHOW_TABLES"),
    ("what tables are there", "SHOW_TABLES"),
    ("list the tables", "SHOW_TABLES"),
    ("show me the list of tables", "SHOW_TABLES"),
]