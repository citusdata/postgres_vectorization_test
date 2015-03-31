Vectorized Executor
===================

I interned at Citus Data this summer, and implemented a vectorized executor for
PostgreSQL. We observed performance improvements of 3-4x for simple SELECT
queries with vectorized execution, and decided to open source my project as a
proof of concept.

This readme first describes the motivation behind my internship, and my journey
with PostgreSQL, database execution engines, and GProf. If you'd like to skip that,
you can also jump to [build instructions](README.md#building).

Motivation
----------

I'm a second year student at Bogazici University in Istanbul, and I interned at
[Citus](http://citusdata.com/). When I started my internship, my mentor Metin 
described to me a common question they were hearing from customers: "I can fit 
my working set into memory, thanks to cheaper RAM, columnar stores, or scaling 
out of data to multiple machines. Now my analytic queries are bottlenecked on 
CPU. Can these queries go faster?"

Since this question's scope was too broad, we decided to pick a simple, yet
useful and representative query. My goal was to go crazy with this (class of)
query's performance. Ideally, my changes would also apply to other queries.

    postgres=# SELECT l_returnflag,
    	       	      sum(l_quantity) as sum_qty,
    		          count(*) as count_order
    	       FROM lineitem
    	       GROUP BY l_returnflag;


Technical Details
-----------------

I started my challenge by compiling PostgreSQL for performance profiling, and
running it with a CPU-profiler called GProf. I then ran our example SELECT
query 25 times to make sure GProf collected enough samples, and looked at the
profile output.

In particular, I was looking for any "hot spot" functions whose behavior I could
understand and change without impacting correctness. For this, I digged down the
GProf call graph, and found the top three functions whose behavior looked
self-contained enough for me to understand.

    index  %time    self  children    called      name
    ...
    [7]     43.3    0.77    17.20    150030375    LookupTupleHashEntry [7]
    [9]     24.0    1.37     8.60    150030375    advance_aggregates [9]
    [17]    12.0    0.26     4.73    150030400    SeqNext [17]

These numbers discouraged me in three ways. First, I was hoping to find a single
function that *was* the performance bottleneck. Instead, PostgreSQL was
spending a proportional amount of time scanning over the lineitem table's tuples
[17], projecting relevant columns from each tuple and grouping them [7], and
applying aggregate functions on grouped values [9].

Second, I read the code for these functions, and found that they were already
optimized. I also found out through profile results that Postgres introduced a
per-tuple overhead. For each tuple, it stored and cleared tuples, dispatched to
relevant executor functions, performed MVCC checks, and so forth.

Third, I understood at a higher level that PostgreSQL was scanning tuples,
projecting columns, and computing aggregates. What I didn't understand was the
dependencies between the thousand other functions involved in query execution.
In fact, whenever I made a change, I spent more time crashing and debugging the
database than the change itself.

To mitigate these issues, we decided to redefine the problem one month into my
internship. To simplify the problem of understanding many internal PostgreSQL
functions, we decided to apply my performance optimizations on the columnar
store extension. This decision had the additional benefit of slashing CPU usage
related to column projections [7].

Then, to speed up tuple scans and aggregate computations, and also to reduce the
per-tuple CPU overhead, we decided to try an interesting idea called vectorized
execution.

[Vectorized execution](http://www.cse.ust.hk/damon2011/proceedings/p5-sompolski.pdf)
was popularized by the [MonetDB/X100](http://oai.cwi.nl/oai/asset/16497/16497B.pdf)
team. This idea is based on the observation that most database engines follow an 
iterator-based execution model, where each database operator implements a next() 
method. Each call to next() produces one new tuple that may in turn be passed to 
other operators. This "tuple at a time" model introduces an interpretation overhead
and also adversely affects high performance features in modern CPUs. Vectorized
execution reduces these overheads by using bulk processing. In this new model,
rather than producing one tuple on each call, next() operates on and produces a
batch of tuples (usually 100-10K).

With the vectorization idea in mind, I started looking into cstore_fdw to see
how I could implement aggregate functions. Sadly, PostgreSQL didn't yet have
aggregate push downs for foreign tables. On the bright side, it provided these
powerful hooks that enabled developers to intercept query planning and execution
logic however they liked.

I started simple this time. I initially overlooked groupings, and implemented
vectorized versions of simple sum(), avg(), and count() on common data types. I
then grabbed the execution hook, and routed any relevant queries to my
vectorized functions.

Next, I generated TPC-H data with a scale factor of 1, loaded the data into the
database, and made sure that data was always *in-memory*. I then ran
simple "Select sum(l_quantity) From lineitem" type queries, and compared the
regular executor to the vectorized version.

<p align="center">
  <img src="images/simple_aggregates.png?raw=true" alt="Run-times for simple aggregates"/>
</p>

The results looked cheerful. Vectorized functions showed performance benefits of
4-6x across different aggregate functions and data types. The simplest of these
functions also had the greatest benefits, plain count(\*). This wasn't all that
surprising. The standard executor was calling count(\*) on one new tuple,
count(\*) was incrementing a counter, and then onto the next tuple. The
vectorized version was instead a simple for() loop over a group of values.

From there, I started looking into queries that aggregated and grouped their
results on one dimension. This time, I made changes to implement vectorized
"hash aggregates", and again routed any relevant queries to my vectorized
functions. I then compared the two executors for simple group bys with
aggregates.

<p align="center">
  <img src="images/groupby_aggregates.png?raw=true" alt="Run-times for group by aggregates"/>
</p>

These results showed performance benefits of around 3x. These improvements were
small in comparison to plain aggregate vectorization because of the generic hash
function's computational overhead. Also, I only had time to implement hash
vectorization for Postgres' pass-by-value data types. In fact, it took me quite
a while to understand that Postgres even had pass-by-value types, and that those
differed from pass-by-reference ones.

If I had the time, I'd look into making my hash aggregate logic more generic.
I'd also try an alternate hashing function that's more specialized, one that
only operates on 1-2 columns, and as a result, that goes faster.

Learnings
---------

In the end, I feel that I learned a lot during my internship at Citus. My first
takeaway was that reading code takes much more time than writing it. In fact,
only after two months of reading code and asking questions, did I start to
understand how the PostgreSQL aggregate logic worked.

Second, it's exciting to try out new ideas in databases. For beginners, the best
way to start is to carve out a specific piece of functionality, find the related
[PostgreSQL extension API](http://www.postgresql.org/docs/9.3/static/extend.html), 
and start implementing against it.

Finally, I'm happy that we're open sourcing my work. I realize that the code
isn't as robust or generic as PostgreSQL is. That said, I know a lot more about
PostgreSQL and love it, and I can only hope that the ideas in here will
stimulate others.


Building
--------

The vectorized execution logic builds on the
[cstore\_fdw](https://github.com/citusdata/cstore_fdw) extension. Therefore,
the dependencies and build steps are exactly the same between the two
extensions. The difference is that cstore\_fdw reduces the amount of disk I/O
by only reading relevant columns and compression data. This extension helps more
when the working set fits into memory. In that case, it reduces the CPU overhead
by vectorizing simple SELECT queries.

My primary goal with this extension was to test vectorization's potential
benefits. As such, we'd love for you to try this out and give us any feedback.
At the same time, please don't consider this extension as generic and
production-ready database code. PostgreSQL does set a high bar there.

cstore\_fdw depends on protobuf-c for serializing and deserializing table metadata.
So we need to install these packages first:

    # Fedora 17+, CentOS, and Amazon Linux
    sudo yum install protobuf-c-devel

    # Ubuntu 10.4+
    sudo apt-get install protobuf-c-compiler
    sudo apt-get install libprotobuf-c0-dev

    # Mac OS X
    brew install protobuf-c

**Note.** In CentOS 5 and 6, you may need to install or update EPEL 5 or EPEL 6
repositories. See [this page]
(http://www.rackspace.com/knowledge_center/article/installing-rhel-epel-repo-on-centos-5x-or-6x)
for instructions.

**Note.** In Amazon Linux, EPEL 6 repository is installed by default, but it is not
enabled. See [these instructions](http://aws.amazon.com/amazon-linux-ami/faqs/#epel)
for how to enable it.

Once you have protobuf-c installed on your machine, you are ready to build
cstore\_fdw.  For this, you need to include the pg\_config directory path in
your make command. This path is typically the same as your PostgreSQL
installation's bin/ directory path. For example:

    PATH=/usr/local/pgsql/bin/:$PATH make
    sudo PATH=/usr/local/pgsql/bin/:$PATH make install

**Note.** postgres_vectorization_test requires PostgreSQL 9.3. It doesn't support 
other versions of PostgreSQL.

Before using cstore\_fdw, you also need to add it to ```shared_preload_libraries```
in your ```postgresql.conf``` and restart Postgres:

    shared_preload_libraries = 'cstore_fdw'    # (change requires restart)

You can use PostgreSQL's ```COPY``` command to load or append data into the table.
You can use PostgreSQL's ```ANALYZE table_name``` command to collect statistics
about the table. These statistics help the query planner to help determine the
most efficient execution plan for each query.


Example
-------

As an example, we demonstrate loading and querying data to/from a column store
table from scratch here. Let's start with downloading and decompressing the data
files.

    wget http://examples.citusdata.com/customer_reviews_1998.csv.gz
    wget http://examples.citusdata.com/customer_reviews_1999.csv.gz

    gzip -d customer_reviews_1998.csv.gz
    gzip -d customer_reviews_1999.csv.gz

Then, let's log into Postgres, and run the following commands to create a column
store foreign table:

```SQL
-- load extension first time after install
CREATE EXTENSION cstore_fdw;

-- create server object
CREATE SERVER cstore_server FOREIGN DATA WRAPPER cstore_fdw;

-- create foreign table
CREATE FOREIGN TABLE customer_reviews
(
    customer_id TEXT,
    review_date DATE,
    review_rating INTEGER,
    review_votes INTEGER,
    review_helpful_votes INTEGER,
    product_id CHAR(10),
    product_title TEXT,
    product_sales_rank BIGINT,
    product_group TEXT,
    product_category TEXT,
    product_subcategory TEXT,
    similar_product_ids CHAR(10)[]
)
SERVER cstore_server;
```

Next, we load data into the table:

```SQL
COPY customer_reviews FROM '/home/user/customer_reviews_1998.csv' WITH CSV;
COPY customer_reviews FROM '/home/user/customer_reviews_1999.csv' WITH CSV;
```

**Note.** If you are getting ```ERROR: cannot copy to foreign table
"customer_reviews"``` when trying to run the COPY commands, double check that you
have added cstore\_fdw to ```shared_preload_libraries``` in ```postgresql.conf```
and restarted Postgres.

Next, we collect data distribution statistics about the table. This is optional,
but usually very helpful:

```SQL
ANALYZE customer_reviews;
```

Finally, let's run some simple SQL queries and see how vectorized execution
performs. We also encourage you to load the same data into a regular PostgreSQL
table, and compare query performance differences.

```SQL
-- Number of customer reviews
SELECT count(*) FROM customer_reviews;

-- Average and total votes for all customer reviews
SELECT avg(review_votes), sum(review_votes) FROM customer_reviews;

-- Total number of helpful votes per product category
SELECT
    product_group, sum(review_helpful_votes) AS total_helpful_votes
FROM
    customer_reviews
GROUP BY
    product_group;

-- Number of reviews by date (year, month, day)
SELECT
    review_date, count(review_date) AS review_count
FROM
    customer_reviews
GROUP BY
    review_date;
```


Limitations
-----------

The vectorized executor intercepts PostgreSQL's query execution hook. If the
extension can't process the current query, it hands the query over to the
standard Postgres executor. Therefore, if your query isn't going any faster,
then we currently don't support vectorization for it.

The current set of vectorized queries are limited to simple aggregates (sum,
count, avg) and aggregates with group bys. The next set of changes I wanted to
incorporate into the vectorized executor are: filter clauses, functions or
expressions, expressions within aggregate functions, groups by that support 
multiple columns or aggregates, and passing vectorized tuples from groupings to 
order by clauses.

I think all except the last one are relatively easy, but I didn't have the time
to work on them. The last one is harder as PostgreSQL's query planner follows
a recursive pull model. In this model, each relational operator is called
recursively to traverse the operator tree from the root downwards, with the
result tuples being pulled upwards. Such a recursion occurs with the aggregate
operator, and I could intercept all operators that are below the aggregate
operator in the query plan tree. If there was an operator on top of the
aggregate operator, such as an order by, I may have had to copy code from the
executor to properly intercept the recursion.


Usage with CitusDB
--------------------

The example above illustrated how to load data into a PostgreSQL database running
on a single host. However, sometimes your data is too large to analyze effectively
on a single host. CitusDB is a product built by Citus Data that allows you to run
a distributed PostgreSQL database to analyze your data using the power of multiple
hosts. CitusDB is based on a modern PostgreSQL version and allows you to easily
install PostgreSQL extensions and foreign data wrappers, including cstore_fdw. For
an example of how to use cstore\_fdw with CitusDB see the
[CitusDB documentation][citus-cstore-docs].


Uninstalling cstore_fdw
-----------------------

Before uninstalling the extension, first you need to drop all the cstore tables:

    postgres=# DROP FOREIGN TABLE cstore_table_1;
    ...
    postgres=# DROP FOREIGN TABLE cstore_table_n;

Then, you should drop the cstore server and extension:

    postgres=# DROP SERVER cstore_server;
    postgres=# DROP EXTENSION cstore_fdw;

cstore\_fdw automatically creates some directories inside the PostgreSQL's data
directory to store its files. To remove them, you can run:

    $ rm -rf $PGDATA/cstore_fdw

Then, you should remove cstore\_fdw from ```shared_preload_libraries``` in
your ```postgresql.conf```:

    shared_preload_libraries = ''    # (change requires restart)

Finally, to uninstall the extension you can run the following command in the
extension's source code directory. This will clean up all the files copied during
the installation:

    $ sudo PATH=/usr/local/pgsql/bin/:$PATH make uninstall


Copyright
---------

Copyright (c) 2014 Citus Data, Inc.

This module is free software; you can redistribute it and/or modify it under the
Apache v2.0 License.

For all types of questions and comments about the wrapper, please contact us at
engage @ citusdata.com.
