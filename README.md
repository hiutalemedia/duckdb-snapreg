# duckdb-snapreg

A DuckDB extension that finds exact rational linear relationships in numeric data.

It combines OLS regression with **rational snapping** — converting floating-point regression output into exact rational hypotheses (`1/3`, `2/1`, `5/4`, ...) and then validating that the resulting outputs are integers. This makes it useful anywhere you expect exact arithmetic relationships to exist in data but are working with measurements that carry floating-point noise.

---

## The Idea

Standard linear regression on 3–5 data points gives you a slope like `0.33333...`. That float is useless on its own. But snap it to `1/3`, and now you have a testable hypothesis: does `feature × (1/3) + intercept` produce an exact integer for every data point?

Rather than regression, snapreg uses a **pairwise candidate search**. Every pair of data points defines an exact slope; every point combined with a candidate slope defines an exact intercept. Each `(slope, intercept)` pair is then validated against all points using exact `__int128` arithmetic. The candidate with fewest failures wins.

```
Points: (x=1,y=3), (x=2,y=5), (x=3,y=7)

Pair (1→2): slope_raw = (5-3)/(2-1) = 2 → snaps to 2/1
Pair (1→3): slope_raw = (7-3)/(3-1) = 2 → same

For slope=2, intercept from point 1: 3 - 2*1 = 1 → snaps to 1/1

Validate (slope=2, intercept=1) against all points:
  2*1+1=3 ✓  2*2+1=5 ✓  2*3+1=7 ✓ → 0 mismatches
```

This approach is robust to high-leverage outliers — a single point with an extreme feature value that would destroy a regression doesn't affect the slope candidates derived from the other pairs. The validation step uses exact `__int128` arithmetic, so there is no floating-point error in the confirmation. A result either passes exactly or it does not.

---

## Functions

### `snap_rational(v DOUBLE, max_den INTEGER)`

Snaps a float to the nearest rational with denominator ≤ `max_den`. Returns `NULL` if no rational is within tolerance.

```sql
SELECT snap_rational(0.3333333, 16);
-- → {'num': 1, 'den': 3}

SELECT snap_rational(1.6666667, 16);
-- → {'num': 5, 'den': 3}

SELECT snap_rational(0.3333333, 2);
-- → NULL  (1/3 needs denominator 3, which exceeds max_den=2)
```

Returns `STRUCT(num BIGINT, den BIGINT)`.

---

### `equation_agg(target, feature, example_id, row_idx, max_den)`

Aggregate function. Group by feature name; one row per feature. Fits a linear equation `target = slope × feature + intercept`, snaps both slope and intercept to rationals, and validates every point with exact arithmetic.

```sql
SELECT
    feat_name,
    eq.slope_num,
    eq.slope_den,
    eq.intercept_num,
    eq.intercept_den,
    eq.consistent,       -- true iff every example has zero mismatches
    eq.mismatches,
    eq.bad_indices       -- row_idx values that failed validation
FROM (
    SELECT feat_name,
           equation_agg(target, feature, example_id, row_idx, 16) AS eq
    FROM feature_library
    GROUP BY feat_name
) t
WHERE eq IS NOT NULL
ORDER BY eq.mismatches ASC, eq.r2 DESC;
```

**Arguments:**

| Column | Type | Description |
|---|---|---|
| `target` | `DOUBLE` | The value to explain |
| `feature` | `DOUBLE` | The candidate predictor |
| `example_id` | `INTEGER` | Which example this row belongs to |
| `row_idx` | `INTEGER` | Caller-assigned row identifier, returned in index lists |
| `max_den` | `INTEGER` | Maximum denominator for snapping (16 is a good default) |

**Result struct:**

| Field | Type | Description |
|---|---|---|
| `slope_num` | `BIGINT` | Slope numerator |
| `slope_den` | `BIGINT` | Slope denominator (always positive) |
| `intercept_num` | `BIGINT` | Intercept numerator |
| `intercept_den` | `BIGINT` | Intercept denominator |
| `mismatches` | `INTEGER` | Total points where validation failed |
| `n_good` | `INTEGER` | Points that passed |
| `n_bad` | `INTEGER` | Points that failed |
| `consistent` | `BOOLEAN` | True iff every example individually has zero mismatches |
| `r2` | `DOUBLE` | OLS R² on the raw floats before snapping |
| `per_example_mm` | `INTEGER[]` | Mismatch count per example, sorted by example_id |
| `good_indices` | `INTEGER[]` | `row_idx` values that passed |
| `bad_indices` | `INTEGER[]` | `row_idx` values that failed |

Returns `NULL` when: fewer than 2 points, feature is constant (degenerate OLS), or no rational snap exists within tolerance for slope or intercept.

**Constant features** (all feature values equal) are handled as slope=0, intercept=mean(target). If the mean snaps cleanly this still returns a valid result — useful for discovering that a hole is simply a constant.

---

### `residual_hint(vals DOUBLE[])`

Analyses a residual vector and returns structural hints. Intended to help an orchestrator decide what to do next with unexplained residuals rather than brute-forcing deeper search.

```sql
SELECT h.*
FROM residual_hint([3.0, 4.0, 4.0, 3.0, 4.0]) h;
```

| Field | Type | Meaning |
|---|---|---|
| `range_tight` | `BOOLEAN` | max − min ≤ 2 |
| `nearly_constant` | `BOOLEAN` | variance < 0.25 |
| `nearly_linear` | `BOOLEAN` | R² vs index > 0.90 |
| `binary_valued` | `BOOLEAN` | exactly 2 distinct rounded values |
| `n_distinct` | `INTEGER` | number of distinct rounded values |
| `min_val` | `DOUBLE` | minimum |
| `max_val` | `DOUBLE` | maximum |
| `mean_val` | `DOUBLE` | mean |

A `binary_valued` residual is a predicate in disguise: `output = base + boolean × range`. Pass `bad_indices` to a predicate solver rather than searching deeper for an equation.

---

## Multi-example consistency

The `consistent` field and `per_example_mm` array exist because a globally zero mismatch count can hide failures that cancel across examples. `consistent = true` means every example independently has zero failures — which is the correct condition for a solution that generalises.

```sql
-- Only accept equations that hold in every example
WHERE eq.consistent = true

-- Or inspect per-example breakdown for partial matches
WHERE list_max(eq.per_example_mm) <= 1
```

---

## Building

```bash
git clone --recurse-submodules https://github.com/duckdb/extension-template duckdb-snapreg
cd duckdb-snapreg

# Copy src/, test/, extension_config.cmake, vcpkg.json from this repo
python3 scripts/bootstrap-template.py equation_search

# Build (add GEN=ninja for faster incremental builds)
make

# Run tests
make test
```

Outputs:

```
build/release/duckdb                                            # shell with extension pre-loaded
build/release/extension/equation_search/equation_search.duckdb_extension
```

---

## Loading

```sql
-- Unsigned local build
-- Start DuckDB with: duckdb -unsigned
LOAD 'build/release/extension/equation_search/equation_search.duckdb_extension';

-- Or once published to community extensions
INSTALL equation_search FROM community;
LOAD equation_search;
```

---

## Design notes

**Why pairwise candidate search instead of regression?**
OLS minimizes squared error globally. One outlier or high-leverage point (a feature value far from the cluster) shifts the slope enough that the snap lands on a wrong rational, causing integer validation to fail everywhere — not just at the outlier. Pairwise search derives slope candidates directly from point pairs, so the correct slope appears as a candidate as long as any two good points exist. Complexity is O(n³) vs O(n) for regression, but for for our use cases n ≤ 20 this is still microseconds.

**`r2` is always 0.0**
The `r2` column is retained for API compatibility but is no longer computed. Pairwise search ranks candidates by mismatch count, not R². Callers should use `mismatches` and `consistent` for quality assessment.

---

## License

MIT