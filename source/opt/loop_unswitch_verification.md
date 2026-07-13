Loop-Unswitch Pass Verification Report
======================================

Files examined:
- C:\dev\DirectXShaderCompiler\external\SPIRV-Tools\source\opt\loop_unswitch_pass.h
- C:\dev\DirectXShaderCompiler\external\SPIRV-Tools\source\opt\loop_unswitch_pass.cpp

Findings:
1. The pass class LoopUnswitchPass derives from Pass and is registered via
   CreateLoopUnswitchPass() factory in optimizer.hpp and optimizer.cpp.

2. The flag --loop-unswitch is correctly registered in RegisterPassFromFlag()
   at line ~402 of optimizer.cpp.

3. The pass handles loop-invariant OpBranchConditional conditions:
   - CanUnswitchLoop() in the LoopUnswitch helper class checks for branches
     (OpBranchConditional and OpSwitch) within the loop that are not constant
     and not loop-invariant (IsConditionNonConstantLoopInvariant).
   - IsConditionNonConstantLoopInvariant() checks:
     a) The condition is not a constant
     b) The condition is not inside the loop
     c) The condition is dynamically uniform (approximated as uniform)

4. The pass clones the loop for each branch:
   - PerformUnswitch() uses LoopUtils::CloneLoop() to create a cloned loop
   - It calls SpecializeLoop() to specialize each clone with the constant value
   - The original loop is also specialized
   - A new selection construct (if/switch) is created outside the loop

5. Key implementation details:
   - Uses structured control flow (create merge blocks)
   - Handles both OpBranchConditional and OpSwitch
   - Preserves LCSSA form by updating phi nodes in merge blocks
   - Updates dominator tree, CFG, and loop descriptor
   - Uses the instruction folder for constant handling

6. Verification: The pass is complete and correctly implemented. No changes needed.
   It handles both if-conditions and switch-statements, clones loops correctly,
   and maintains proper structured control flow.

Conclusion: Pass 14 (loop-unswitch) is already fully implemented and correctly
registered. No action required.
