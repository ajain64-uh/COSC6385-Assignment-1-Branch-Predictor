# Pentium M Hybrid Branch Predictor (CBP2 infrastructure)

### Github: https://github.com/ajain64-uh/COSC6385-Assignment-1-Branch-Predictor

Assignment 1: implementation of the Pentium M branch **outcome** predictor
(bimodal table + set-associative global predictor) as the `pm_predictor`
class in `src/my_predictor.h`. The loop predictor and target prediction are
skipped, as the assignment allows.

**Extra credit:** the complete Pentium M branch prediction unit (PIR, BTB,
iBTB, loop predictor, bimodal and global predictors, outcome and target
prediction) is implemented as the `cpm_predictor` class in the same file

## What was changed / added

| File | Purpose |
|---|---|
| `src/my_predictor.h` | **The submission.** Implements the member functions of the skeleton's `pm_update` and `pm_predictor` classes **Extra credit:** implements `cpm_update` and `cpm_predictor` (complete Pentium M). `gshare_predictor` is untouched. |
| `src/predict.cc` | Changes to line 32 in order to test each preditor. Exact code mentioned below |

In order to test each configuration, the class object in predict.cc needs to be updated at line number 32.

`branch_predictor *p = new gshare_predictor ();` // For Base GShare Predictor

`branch_predictor *p = new pm_predictor ();` // For Assignment Pentium M Predictor

`branch_predictor *p = new cpm_predictor ();` // For Extra Credit Complete Pentium M Predictor

Each time the above line of code is changed, the following code must be rerun to generate a new run file:

`cd src`
`make`
`cd ..`

Predictors can be tested with the command `./run traces` in main directory

Results of trace run in Report.

Code was executed using Ubuntu running on WSL on Windows 11.