// CoinUtils headers
#include "CoinUtility.hpp"

// Osi headers
#include <OsiRowCut.hpp>

// Disco headers
#include "DcoTreeNode.hpp"
#include "DcoNodeDesc.hpp"
#include "DcoMessage.hpp"
#include "DcoLinearConstraint.hpp"
#include "DcoConicConstraint.hpp"
#include "Dco.hpp"
#include "DcoConGenerator.hpp"
#include "DcoSolution.hpp"
#include "DcoBranchObject.hpp"
#include "DcoHeuristic.hpp"
#include "DcoHeurRounding.hpp"

// STL headers
#include <vector>

extern std::map<DISCO_Grumpy_Msg_Type, char const *> grumpyMessage;
extern std::map<DcoNodeBranchDir, char> grumpyDirection;

// define structs that will be used in creating nodes.
struct SparseVector {
  int * ind;
  double * val;
};

struct Bound {
  SparseVector lower;
  SparseVector upper;
};


DcoTreeNode::DcoTreeNode() {
}

DcoTreeNode::DcoTreeNode(AlpsNodeDesc *& desc) {
  desc_ = desc;
  desc = NULL;
}

DcoTreeNode::~DcoTreeNode() {
}

// create tree node from given description
AlpsTreeNode * DcoTreeNode::createNewTreeNode(AlpsNodeDesc *& desc) const {
  DcoNodeDesc * dco_node = dynamic_cast<DcoNodeDesc*>(desc);
  int branched_index = dco_node->getBranchedInd();
  double branched_value = dco_node->getBranchedVal();

  DcoModel * model = dynamic_cast<DcoModel*>(broker_->getModel());

  double dist_to_floor = branched_value - floor(branched_value);
  double dist_to_ceil = ceil(branched_value) - branched_value;
  double frac = (dist_to_floor>dist_to_ceil) ? dist_to_ceil : dist_to_floor;
  double tol = model->dcoPar()->entry(DcoParams::integerTol);

  if (frac < tol) {
    model->dcoMessageHandler_->message(DISCO_NODE_BRANCHONINT,
                                       *(model->dcoMessages_))
      << branched_index << CoinMessageEol;
  }

  // Create a new tree node
  DcoTreeNode * node = new DcoTreeNode(desc);
  node->setBroker(broker_);
  desc = NULL;
  return node;
}

void DcoTreeNode::convertToExplicit() {
  DcoNodeDesc * node_desc = dynamic_cast<DcoNodeDesc*>(getDesc());
  DcoModel * model = dynamic_cast<DcoModel*>(broker_->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;

  if (explicit_) {
    return;
  }
  // debug stuff
  std::stringstream debug_msg;
  debug_msg << "Proc["
            << broker_->getProcRank()
            << "] converting "
            << this
            << " to explicit.";
  message_handler->message(0, "Dco", debug_msg.str().c_str(),
                           'G', DISCO_DLOG_MPI)
    << CoinMessageEol;
  // end of debug stuff

  explicit_ = 1;

  int num_cols = model->solver()->getNumCols();
  // this will keep bound information, lower and upper
  Bound hard_bound;
  hard_bound.lower.ind = new int[num_cols];
  hard_bound.lower.val = new double[num_cols];
  hard_bound.upper.ind = new int[num_cols];
  hard_bound.upper.val = new double[num_cols];
  std::fill_n(hard_bound.lower.val, num_cols, ALPS_DBL_MAX);
  std::fill_n(hard_bound.upper.val, num_cols, -ALPS_DBL_MAX);

  Bound soft_bound;
  soft_bound.lower.ind = new int[num_cols];
  soft_bound.lower.val = new double[num_cols];
  soft_bound.upper.ind = new int[num_cols];
  soft_bound.upper.val = new double[num_cols];
  std::fill_n(soft_bound.lower.val, num_cols, ALPS_DBL_MAX);
  std::fill_n(soft_bound.upper.val, num_cols, -ALPS_DBL_MAX);

  for (int k = 0; k < num_cols; ++k) {
    hard_bound.lower.ind[k] = k;
    hard_bound.upper.ind[k] = k;
    soft_bound.lower.ind[k] = k;
    soft_bound.upper.ind[k] = k;
  }

  //--------------------------------------------------
  // Travel back to a full node, then collect diff (add/rem col/row,
  // hard/soft col/row bounds) from the node full to this node.
  //--------------------------------------------------------

  AlpsTreeNode * parent = parent_;

  std::vector<AlpsTreeNode*> leafToRootPath;
  leafToRootPath.push_back(this);

  while(parent) {
    leafToRootPath.push_back(parent);
    if (parent->getExplicit()) {
      // Reach an explicit node, then stop.
      break;
    }
    else {
      parent = parent->getParent();
    }
  }

  //------------------------------------------------------
  // Travel back from this node to the explicit node to
  // collect full description.
  //------------------------------------------------------
  for(int i=static_cast<int> (leafToRootPath.size()-1); i>-1; --i) {
    DcoNodeDesc * curr = dynamic_cast<DcoNodeDesc*>((leafToRootPath.at(i))->
                                           getDesc());
    //--------------------------------------
    // Full variable hard bounds.
    //--------------------------------------
    int numModify = curr->getVars()->lbHard.numModify;
    for (int k=0; k<numModify; ++k) {
      int index = curr->getVars()->lbHard.posModify[k];
      double value = curr->getVars()->lbHard.entries[k];
      hard_bound.lower.val[index] = value;
    }

    numModify = curr->getVars()->ubHard.numModify;
    for (int k=0; k<numModify; ++k) {
      int index = curr->getVars()->ubHard.posModify[k];
      double value = curr->getVars()->ubHard.entries[k];
      hard_bound.upper.val[index] = value;
    }

    //--------------------------------------
    // Full variable soft bounds.
    //--------------------------------------
    numModify = curr->getVars()->lbSoft.numModify;
    for (int k=0; k<numModify; ++k) {
      int index = curr->getVars()->lbSoft.posModify[k];
      double value = curr->getVars()->lbSoft.entries[k];
      soft_bound.lower.val[index] = value;
    }

    numModify = curr->getVars()->ubSoft.numModify;
    for (int k=0; k<numModify; ++k) {
      int index = curr->getVars()->ubSoft.posModify[k];
      double value = curr->getVars()->ubSoft.entries[k];
      soft_bound.upper.val[index] = value;
    }



  } // EOF for (path)

  //------------------------------------------
  // Record hard variable bounds. FULL set.
  //------------------------------------------
  node_desc->assignVarHardBound(num_cols,
                                hard_bound.lower.ind,
                                hard_bound.lower.val,
                                num_cols,
                                hard_bound.upper.ind,
                                hard_bound.upper.val);

  //------------------------------------------
  // Recode soft variable bound. Modified.
  //------------------------------------------
  int numSoftVarLowers=0;
  int numSoftVarUppers=0;
  for (int k=0; k<num_cols; ++k) {
    if (soft_bound.lower.val[k] < ALPS_BND_MAX) {
      soft_bound.lower.ind[numSoftVarLowers] = k;
      soft_bound.lower.val[numSoftVarLowers++] = soft_bound.lower.val[k];
    }
    if (soft_bound.upper.val[k] > -ALPS_BND_MAX) {
      soft_bound.upper.ind[numSoftVarUppers] = k;
      soft_bound.upper.val[numSoftVarUppers++] = soft_bound.upper.val[k];
    }
  }
  // Assign it anyway so to delete memory(fVarSoftLBInd,etc.)
  node_desc->assignVarSoftBound(numSoftVarLowers,
                                soft_bound.lower.ind,
                                soft_bound.lower.val,
                                numSoftVarUppers,
                                soft_bound.upper.ind,
                                soft_bound.upper.val);

  //--------------------------------------------------
  // Clear path vector.
  //--------------------------------------------------
  leafToRootPath.clear();
  assert(leafToRootPath.size() == 0);

}

void DcoTreeNode::convertToRelative() {
  DcoNodeDesc * node_desc = dynamic_cast<DcoNodeDesc*>(getDesc());
  DcoModel * model = dynamic_cast<DcoModel*>(broker()->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;
  message_handler->message(DISCO_NOT_IMPLEMENTED, *messages)
    << __FILE__ << __LINE__ << CoinMessageEol;
  throw std::exception();
}

/// Generate constraints for the problem.
int DcoTreeNode::generateConstraints(BcpsConstraintPool * conPool) {
  // todo(aykut) why do we need this status?
  DcoModel * disco_model = dynamic_cast<DcoModel*>(broker_->getModel());
  CoinMessageHandler * message_handler = disco_model->dcoMessageHandler_;
  CoinMessages * messages = disco_model->dcoMessages_;

  // number of constraint generators in model
  int num_cg = disco_model->numConGenerators();
  for (int i=0; i<num_cg; ++i) {
    bool do_use = false;
    DcoConGenerator * cg = disco_model->conGenerators(i);
    // decide whether we should use this cut generator with respect to the
    // specified cut strategy
    decide_using_cg(do_use, cg);
    if (!do_use) {
      // jump to the next geenrator if we will not use this one.
      continue;
    }
    int pre_num_cons = conPool->getNumConstraints();
    double start_time = CoinCpuTime();
    // Call constraint generator
    bool must_resolve = cg->generateConstraints(*conPool);
    double cut_time = CoinCpuTime() - start_time;
    // Statistics
    cg->stats().addTime(cut_time);
    cg->stats().addNumCalls(1);
    int num_cons_generated = conPool->getNumConstraints() - pre_num_cons;
    if (num_cons_generated == 0) {
      cg->stats().addNumNoConsCalls(1);
    }
    else {
      cg->stats().addNumConsGenerated(num_cons_generated);
    }

    // debug msg
    std::stringstream debug_msg;
    debug_msg << "Called " << cg->name() << ", generated "
              << num_cons_generated << " cuts in "
        << cut_time << " seconds.";
    message_handler->message(0, "Dco", debug_msg.str().c_str(),
                             'G', DISCO_DLOG_CUT)
      << CoinMessageEol;
    // end of debug

  }
  // return value will make sense when DcoTreeNode::process is implemented in Bcps level.
  return 0;
}


//todo(aykut) if all columns are feasible, fix columns and call IPM. If the
// resulting solution is better update UB. Fathom otherwise.
void DcoTreeNode::decide_using_cg(bool & do_use,
                                  DcoConGenerator const * cg) const {
  DcoModel * model = dynamic_cast<DcoModel*> (broker_->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;
  DcoCutStrategy strategy = cg->strategy();
  // todo(aykut) make it a parameter, Only autmatic stategy has depth limit.
  int maxConstraintDepth = 20;

  do_use = false;

  switch (strategy) {
  case DcoCutStrategyNone:
    do_use = false;
    break;
  case DcoCutStrategyRoot:
    if (depth_==0) {
      do_use = true;
    }
    break;
  case DcoCutStrategyAuto:
    if (depth_ < maxConstraintDepth) {
      if (!diving_ || depth_==0) {
        do_use = true;
      }

    }
    break;
  case DcoCutStrategyPeriodic:
    if (index_ % cg->frequency() == 0) {
      do_use = true;
    }
    break;
  default:
    message_handler->message(DISCO_UNKNOWN_CUTSTRATEGY, *messages)
      << strategy << CoinMessageEol;
  }
}


int DcoTreeNode::generateVariables(BcpsVariablePool * varPool) {
  DcoModel * disco_model = dynamic_cast<DcoModel*>(broker_->getModel());
  CoinMessageHandler * message_handler = disco_model->dcoMessageHandler_;
  CoinMessages * messages = disco_model->dcoMessages_;
  message_handler->message(DISCO_NOT_IMPLEMENTED, *messages)
    << __FILE__ << __LINE__ << CoinMessageEol;
  throw std::exception();
  return 0;
}

int DcoTreeNode::chooseBranchingObject() {
  DcoModel * disco_model = dynamic_cast<DcoModel*>(broker_->getModel());
  CoinMessageHandler * message_handler = disco_model->dcoMessageHandler_;
  CoinMessages * messages = disco_model->dcoMessages_;
  message_handler->message(DISCO_NOT_IMPLEMENTED, *messages)
    << __FILE__ << __LINE__ << CoinMessageEol;
  throw std::exception();
  return 0;
}

int DcoTreeNode::process(bool isRoot, bool rampUp) {
  AlpsNodeStatus status = getStatus();
  DcoModel * model = dynamic_cast<DcoModel*>(broker()->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;

  // debug stuff
  std::stringstream debug_msg;
  debug_msg << "Processing node ";
  debug_msg << this;
  debug_msg << " index ";
  debug_msg << getIndex();
  debug_msg << " parent ";
  debug_msg << getParent();
  message_handler->message(0, "Dco", debug_msg.str().c_str(),
                           'G', DISCO_DLOG_PROCESS)
    << CoinMessageEol;
  // end of debug stuff

  // check if this can be fathomed
  double tailoff = model->dcoPar()->entry(DcoParams::tailOff);
  if (getQuality() > broker()->getBestQuality() - tailoff) {
    // debug message
    message_handler->message(0, "Dco", "Node fathomed due to parent quality.",
                             'G', DISCO_DLOG_PROCESS);
    // end of debug message
    // grumpy message
    model->dcoMessageHandler_->message(DISCO_GRUMPY_MESSAGE_MED,
                                       *model->dcoMessages_)
      << broker()->timer().getTime()
      << grumpyMessage[DISCO_GRUMPY_FATHOMED]
      << getIndex()
      << getParentIndex()
      << grumpyDirection[dynamic_cast<DcoNodeDesc*>(desc_)->getBranchedDir()]
      << model->objSense()*getQuality()
      << CoinMessageEol;
    // end of grumpy message
    setStatus(AlpsNodeStatusFathomed);
    return AlpsReturnStatusOk;
  }

  if (status==AlpsNodeStatusCandidate or
      status==AlpsNodeStatusEvaluated) {
    boundingLoop(isRoot, rampUp);
  }
  else if (status==AlpsNodeStatusBranched or
           status==AlpsNodeStatusFathomed or
           status==AlpsNodeStatusDiscarded) {
    // this should not happen
    message_handler->message(DISCO_NODE_UNEXPECTEDSTATUS, *messages)
      << static_cast<int>(status) << CoinMessageEol;
  }
  return AlpsReturnStatusOk;
}

int DcoTreeNode::boundingLoop(bool isRoot, bool rampUp) {
  AlpsNodeStatus status = getStatus();
  DcoNodeDesc * desc = getDesc();
  DcoModel * model = dynamic_cast<DcoModel*>(broker_->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;
  bool keepBounding = true;
  bool fathomed = false;
  bool do_branch = false;
  bool genConstraints = false;
  bool genVariables = false;
  BcpsConstraintPool * constraintPool = new BcpsConstraintPool();
  BcpsVariablePool * variablePool = new BcpsVariablePool();
  installSubProblem();

  while (keepBounding) {
    keepBounding = false;
    // solve subproblem corresponds to this node
    BcpsSubproblemStatus subproblem_status = bound();

    // debug print objective value after bounding
    std::stringstream debug_msg;
    debug_msg << "Subproblem solved. "
              << "status "
              << subproblem_status
              << " Obj value "
              << quality_
              << " estimate "
              << solEstimate_;
    message_handler->message(0, "Dco", debug_msg.str().c_str(),
                             'G', DISCO_DLOG_PROCESS);
    // end of debug stuff

    // grumpy message
    if (subproblem_status==BcpsSubproblemStatusOptimal &&
        getStatus()==AlpsNodeStatusCandidate or
        getStatus()==AlpsNodeStatusEvaluated) {
      double sum_inf = 0.0;
      for (int i=0; i<model->branchStrategy()->numBranchObjects(); ++i) {
        sum_inf += model->branchStrategy()->branchObjects()[i]->value();
      }
      message_handler->message(DISCO_GRUMPY_MESSAGE_LONG, *messages)
        << broker()->timer().getTime()
        << grumpyMessage[DISCO_GRUMPY_CANDIDATE]
        << getIndex()
        << getParentIndex()
        << grumpyDirection[dynamic_cast<DcoNodeDesc*>(desc_)->getBranchedDir()]
        << model->objSense()*getQuality()
        << sum_inf
        << model->branchStrategy()->numBranchObjects()
        << CoinMessageEol;
    }
    // end of grumpy message

    // call heuristics to search for a solution
    callHeuristics();

    // decide what to do
    branchConstrainOrPrice(subproblem_status, keepBounding, do_branch,
                           genConstraints,
                           genVariables);

    // debug message
    // reset debug message
    debug_msg.str(std::string());
    debug_msg << "BCP function decided to"
              << " keep bounding "
              << keepBounding
              << " branch "
              << do_branch
              << " generate cons "
              << genConstraints;
    message_handler->message(0, "Dco", debug_msg.str().c_str(),
                             'G', DISCO_DLOG_PROCESS);
    // end of debug stuff

    if (getStatus()==AlpsNodeStatusFathomed) {
      // node is fathomed, nothing to do.
      break;
    }
    else if (keepBounding and genConstraints) {
      generateConstraints(constraintPool);
      // add constraints to the model
      applyConstraints(constraintPool);
      // clear constraint pool
      constraintPool->freeGuts();
      // set status to evaluated
      setStatus(AlpsNodeStatusEvaluated);
    }
    else if (keepBounding and genVariables) {
      generateVariables(variablePool);
      // add variables to the model
      // set status to evaluated
      setStatus(AlpsNodeStatusEvaluated);
    }
    else if (keepBounding==false and do_branch==false) {
      // put node back into the list.
      // this means do not change the node status and end processing the node.
      // set status to evaluated
      setStatus(AlpsNodeStatusEvaluated);
    }
    else if (keepBounding==false and do_branch) {
      // branch
      BcpsBranchStrategy * branchStrategy = model->branchStrategy();
      // todo(aykut) following should be a parameter
      // Maximum number of resolve during branching.
      int numBranchResolve = 10;
      // todo(aykut) why ub should be an input?
      branchStrategy->createCandBranchObjects(this);
      // prepare this node for branching, bookkeeping for differencing.
      // call pregnant setting routine
      processSetPregnant();
    }
    else {
      message_handler->message(9998, "Dco", "This should not happen. "
                               " branchConstrainOrPrice() is buggy.", 'E', 0)
        << CoinMessageEol;
    }

  }
  delete constraintPool;
  delete variablePool;
  return AlpsReturnStatusOk;
}

void DcoTreeNode::callHeuristics() {
  AlpsNodeStatus status = getStatus();
  DcoNodeDesc * desc = getDesc();
  DcoModel * model = dynamic_cast<DcoModel*>(broker()->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;
  int num_heur = model->numHeuristics();
  DcoSolution * sol;
  for (int i=0; i<num_heur; ++i) {
    DcoHeuristic * curr = model->heuristics(i);
    sol = curr->searchSolution();
    if (sol) {
      // set depth
      setDepth(depth_);
      // set index
      setIndex(index_);
      model->storeSolution(sol);
      // debug log
      message_handler->message(DISCO_HEUR_SOL_FOUND, *messages)
        << curr->name()
        << sol->getQuality();
    }
    else {
      // debug message
      message_handler->message(DISCO_HEUR_NOSOL_FOUND, *messages)
        << curr->name();
    }
  }
}

/// Bounding procedure to estimate quality of this node.
BcpsSubproblemStatus DcoTreeNode::bound() {
  BcpsSubproblemStatus subproblem_status;
  DcoModel * model = dynamic_cast<DcoModel*>(broker_->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;
  AlpsNodeStatus node_status = getStatus();
  if (node_status==AlpsNodeStatusPregnant or
      node_status==AlpsNodeStatusBranched or
      node_status==AlpsNodeStatusFathomed or
      node_status==AlpsNodeStatusDiscarded) {
    message_handler->message(DISCO_NODE_UNEXPECTEDSTATUS, *messages)
      << static_cast<int>(node_status) << CoinMessageEol;
  }
  // solve problem loaded to the solver
  model->solver()->resolve();
  if (model->solver()->isAbandoned()) {
    subproblem_status = BcpsSubproblemStatusAbandoned;
  }
  else if (model->solver()->isProvenOptimal()) {
    // todo(aykut) if obj val is greater than 1e+30 we consider problem
    // infeasible. This is due to our cut generation when the problem is
    // infeasible. We add a high lower bound (1e+30) to the objective
    // function. This can be improved.
    if (model->solver()->getObjValue()>=1e+30) {
      subproblem_status = BcpsSubproblemStatusPrimalInfeasible;
      quality_ = ALPS_OBJ_MAX;
      solEstimate_ = ALPS_OBJ_MAX;
    }
    else {
      subproblem_status = BcpsSubproblemStatusOptimal;
      double objValue = model->solver()->getObjValue() *
        model->solver()->getObjSense();
      // Update quality of this node
      quality_ = objValue;
      solEstimate_ = objValue;
    }
  }
  else if (model->solver()->isProvenPrimalInfeasible()) {
    subproblem_status = BcpsSubproblemStatusPrimalInfeasible;
  }
  else if (model->solver()->isProvenDualInfeasible()) {
    subproblem_status = BcpsSubproblemStatusDualInfeasible;
  }
  else if (model->solver()->isPrimalObjectiveLimitReached()) {
    subproblem_status = BcpsSubproblemStatusPrimalObjLim;
  }
  else if (model->solver()->isDualObjectiveLimitReached()) {
    subproblem_status = BcpsSubproblemStatusDualObjLim;
  }
  else if (model->solver()->isIterationLimitReached()) {
    subproblem_status = BcpsSubproblemStatusIterLim;
  }
  else {
    message_handler->message(DISCO_SOLVER_UNKNOWN_STATUS, *messages)
      << CoinMessageEol;
  }
  return subproblem_status;
}

int DcoTreeNode::installSubProblem() {
  // load the problem in this node to the solver
  //
  //
  //======================================================
  // Restore subproblem:
  //  1. Remove noncore columns and rows
  //  2. Travel back to root and correct differencing to
  //     full column/row bounds into model->startXXX
  //  3. Set col bounds
  //  4. Set row bounds (is this necessary?)
  //  5. Add contraints except cores
  //  6. Add variables except cores
  //  7. Set basis (should not need modify)
  //======================================================
  AlpsReturnStatus status = AlpsReturnStatusOk;
  DcoModel * model = dynamic_cast<DcoModel*>(broker_->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;
  DcoNodeDesc * desc = dynamic_cast<DcoNodeDesc*>(getDesc());
  // get number of columns and rows
  int numCoreCols = model->getNumCoreVariables();
  int numCoreLinearRows = model->getNumCoreLinearConstraints();
  int numCoreConicRows = model->getNumCoreConicConstraints();
  // get number of columns and rows stored in the solver
  int numSolverCols = model->solver()->getNumCols();
  // solver rows are all linear, for both Osi and OsiConic.
  int numSolverRows = model->solver()->getNumRows();

  // 1. Remove noncore columns and rows
  // 1.1 Remove non-core rows from solver, i.e. cuts
  int numDelRows = numSolverRows - numCoreLinearRows;
  if (numDelRows > 0) {
    int * indices = new int[numDelRows];
    if (indices==NULL) {
      message_handler->message(DISCO_OUT_OF_MEMORY, *messages)
        << __FILE__ << __LINE__ << CoinMessageEol;
      throw CoinError("Out of memory", "installSubProblem", "DcoTreeNode");
    }
    for (int i=0; i<numDelRows; ++i) {
      indices[i] = numCoreLinearRows + i;
    }
    model->solver()->deleteRows(numDelRows, indices);
    delete[] indices;
    indices = NULL;
  }
  // 1.1 Remove non-core columns from solver
  // End of 1.

  //  2. Travel back to root and correct differencing to full column/row bounds
  // into model->startXXX
  //--------------------------------------------------------
  // Travel back to a full node, then collect diff (add/rem col/row,
  // hard/soft col/row bounds) from the node full to this node.
  //----------------------------
  // Note: if we store full set of logic/agorithm col/row, then
  //       no diff are needed for col/row
  //--------------------------------------------------------
  //--------------------------------------------------------
  // Collect differencing bounds. Branching bounds of this node
  // are ALSO collected.
  //--------------------------------------------------------
  /* First push this node since it has branching hard bounds.
     NOTE: during rampup, this desc has full description when branch(). */

  // column and row lower bounds
  // todo(aykut) why do we need to write them to model->colLB_ fields?
  // is it enough to have a local array?
  // this creates a bug unless you restore the bounds to original values.
  // this should be fixed.
  double * colLB = model->colLB();
  double * colUB = model->colUB();
  CoinFillN(colLB, numCoreCols, -ALPS_DBL_MAX);
  CoinFillN(colUB, numCoreCols, ALPS_DBL_MAX);
  // generate path to root from this
  std::vector<AlpsTreeNode*> leafToRootPath;
  leafToRootPath.push_back(this);
  if (broker_->getPhase() != AlpsPhaseRampup) {
    AlpsTreeNode * parent = parent_;
    while(parent) {
      leafToRootPath.push_back(parent);
      if (parent->getExplicit()) {
        // Reach an explicit node, then stop.
        break;
      }
      else {
        parent = parent->getParent();
      }
    }
  }
  //------------------------------------------------------
  // Travel back from this node to the explicit node to
  // collect full description.
  //------------------------------------------------------
  int numOldRows = 0;
  std::vector<DcoConstraint*> old_cons;
  for(int i = static_cast<int> (leafToRootPath.size() - 1); i > -1; --i) {
    //--------------------------------------------------
    // NOTE: As away from explicit node, bounds become
    //       tighter and tighter.
    //--------------------------------------------------
    // node description of node i
    DcoNodeDesc * currDesc =
      dynamic_cast<DcoNodeDesc*>((leafToRootPath.at(i))->getDesc());
    //--------------------------------------------------
    // Adjust bounds according to hard var lb/ub.
    // If rampup or explicit, collect hard bounds so far.
    //--------------------------------------------------
    int index;
    double value;
    int numModify;
    numModify = currDesc->getVars()->lbHard.numModify;
    for (int k=0; k<numModify; ++k) {
      index = currDesc->getVars()->lbHard.posModify[k];
      value = currDesc->getVars()->lbHard.entries[k];
      // Hard bounds do NOT change according to soft bounds, so
      // here need CoinMax.
      colLB[index] = CoinMax(colLB[index], value);
    }
    numModify = currDesc->getVars()->ubHard.numModify;
    for (int k=0; k<numModify; ++k) {
      int index = currDesc->getVars()->ubHard.posModify[k];
      double value = currDesc->getVars()->ubHard.entries[k];
      colUB[index] = CoinMin(colUB[index], value);
    }
    //--------------------------------------------------
    // Adjust bounds according to soft var lb/ub.
    // If rampup or explicit, collect soft bounds so far.
    //--------------------------------------------------
    numModify = currDesc->getVars()->lbSoft.numModify;
    for (int k=0; k<numModify; ++k) {
      index = currDesc->getVars()->lbSoft.posModify[k];
      value = currDesc->getVars()->lbSoft.entries[k];
      colLB[index] = CoinMax(colLB[index], value);
    }
    numModify = currDesc->getVars()->ubSoft.numModify;
    for (int k=0; k<numModify; ++k) {
      index = currDesc->getVars()->ubSoft.posModify[k];
      value = currDesc->getVars()->ubSoft.entries[k];
      colUB[index] = CoinMin(colUB[index], value);
    }
    //--------------------------------------------------
    // TODO: Modify hard/soft row lb/ub.
    //--------------------------------------------------
    //--------------------------------------------------
    // Collect active non-core constraints at parent.
    //--------------------------------------------------
    //----------------------------------------------
    // First collect all generated cuts, then remove
    // deleted.
    //----------------------------------------------
    int tempInt = currDesc->getCons()->numAdd;
    for (int k=0; k<tempInt; ++k) {
      DcoConstraint * aCon = dynamic_cast<DcoConstraint *>
        (currDesc->getCons()->objects[k]);
      old_cons.push_back(aCon);
    }
    //----------------------------------------------
    // Remove those deleted.
    // NOTE: old_cons stores all previously
    // generated active constraints at parent.
    //----------------------------------------------
    tempInt = currDesc->getCons()->numRemove;
    if (tempInt > 0) {
      int tempPos;
      int * tempMark = new int [numOldRows];
      CoinZeroN(tempMark, numOldRows);
      for (int k=0; k<tempInt; ++k) {
        tempPos = currDesc->getCons()->posRemove[k];
        tempMark[tempPos] = 1;
      }
      tempInt = 0;
      for (int k=0; k<numOldRows; ++k) {
        if (tempMark[k] != 1) {
          // Survived.
          old_cons[tempInt++] = old_cons[k];
        }
      }
      if (tempInt + currDesc->getCons()->numRemove != numOldRows) {
        std::cout << "INSTALL: tempInt=" << tempInt
                  <<", numRemove="<<currDesc->getCons()->numRemove
                  << ", numOldRows=" << numOldRows << std::endl;
        assert(0);
      }
      // Update number of old non-core constraints.
      numOldRows = tempInt;
      delete [] tempMark;
    }
  } // EOF leafToRootPath.
  //--------------------------------------------------------
  // Clear path vector.
  //--------------------------------------------------------
  leafToRootPath.clear();
  assert(leafToRootPath.size() == 0);
  // End of 2

  //  3. Set col bounds
  //--------------------------------------------------------
  // Adjust column bounds in lp solver
  //--------------------------------------------------------
  model->solver()->setColLower(colLB);
  model->solver()->setColUpper(colUB);
  // End of 3


  //  4. Set row bounds (is this necessary?)

  //  5. Add contraints except cores
  //--------------------------------------------------------
  // TODO: Set row bounds
  //--------------------------------------------------------
  //--------------------------------------------------------
  // Add old constraints, which are collect from differencing.
  //--------------------------------------------------------
  // If removed cuts due to local cuts.
  if (numOldRows > 0) {
    const OsiRowCut ** oldOsiCuts = new const OsiRowCut * [numOldRows];
    for (int k=0; k<numOldRows; ++k) {
      OsiRowCut * acut = old_cons[k]->createOsiRowCut(model);
      oldOsiCuts[k] = acut;
    }
    model->solver()->applyRowCuts(numOldRows, oldOsiCuts);
    for (int k=0; k<numOldRows; ++k) {
      delete oldOsiCuts[k];
    }
    delete [] oldOsiCuts;
    oldOsiCuts = NULL;
  }
  old_cons.clear();
  //  End of 5


  //  6. Add variables except cores

  //  7. Set basis (should not need modify)
  //--------------------------------------------------------
  // Add parent variables, which are collect from differencing.
  //--------------------------------------------------------
  //--------------------------------------------------------
  // Set basis
  //--------------------------------------------------------
  CoinWarmStartBasis *pws = desc->getBasis();
  if (pws != NULL) {
    model->solver()->setWarmStart(pws);
  }
  return status;
  //  End of 7
}

/** This method must be invoked on a \c pregnant node (which has all the
    information needed to create the children) and should create the
    children's decriptions. The stati of the children
    can be any of the ones \c process() can return. */
std::vector< CoinTriple<AlpsNodeDesc*, AlpsNodeStatus, double> >
DcoTreeNode::branch() {
  // get model the node belongs
  DcoModel * model = dynamic_cast<DcoModel*>(broker()->getModel());
  // get message handler and messages for loging
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;

  // check node status, this should be a pregnant node.
  if (getStatus()!=AlpsNodeStatusPregnant) {
      message_handler->message(DISCO_NODE_UNEXPECTEDSTATUS, *messages)
        << static_cast<int>(getStatus()) << CoinMessageEol;
  }

  // create return value and push the down and up nodes.
  std::vector< CoinTriple<AlpsNodeDesc*, AlpsNodeStatus, double> > res;
  // fathom if quality is bad
  if (quality_ > broker()->getBestQuality()) {
    // clear stored string
    message_handler->message(0, "Dco", "Bad quality. No need for "
                             "branching, fathom.",
                             'G', DISCO_DLOG_PROCESS)
      << CoinMessageEol;
    setStatus(AlpsNodeStatusFathomed);
    return res;
  }
  //todo(aykut) update scores
  //BcpsBranchStrategy * branchStrategy = model->branchStrategy();

  // get Alps phase
  AlpsPhase phase = broker()->getPhase();

  // get branch object
  DcoBranchObject const * branch_object =
    dynamic_cast<DcoBranchObject const *>(branchObject());

  if (branch_object==NULL) {
    // branch
    BcpsBranchStrategy * branchStrategy = model->branchStrategy();
    // todo(aykut) following should be a parameter
    // Maximum number of resolve during branching.
    int numBranchResolve = 10;
    branchStrategy->createCandBranchObjects(this);

    // fathom if no branch object found
    if (branchStrategy->numBranchObjects()==0) {
      // clear stored string
      message_handler->message(0, "Dco",
                               "No branch objects found, fathom.",
                               'G', DISCO_DLOG_PROCESS)
        << CoinMessageEol;
      setStatus(AlpsNodeStatusFathomed);
      return res;
    }
    // prepare this node for branching, bookkeeping for differencing.
    // call pregnant setting routine
    //processSetPregnant();
  }

  assert(branch_object);
  // get index and value of branch variable.
  //int branch_var = model->relaxedCols()[branch_object->getObjectIndex()];
  int branch_var = branch_object->index();
  double branch_value = branch_object->value();

  // debug stuff
  std::stringstream debug_msg;
  debug_msg << "Branching node "
            << this
            << " variable "
            << branch_var
            << " value "
            << branch_value;
  message_handler->message(0, "Dco", debug_msg.str().c_str(),
                           'G', DISCO_DLOG_BRANCH)
    << CoinMessageEol;
  // end of debug stuff

  // compute child nodes' warm start basis
  CoinWarmStartBasis * child_ws;
  child_ws = (getDesc()->getBasis()==0) ? 0 :
    new CoinWarmStartBasis(*getDesc()->getBasis());

  // create new node descriptions
  DcoNodeDesc * down_node = new DcoNodeDesc(model);
  down_node->setBroker(broker_);
  DcoNodeDesc * up_node = new DcoNodeDesc(model);
  up_node->setBroker(broker_);
  if (phase == AlpsPhaseRampup) {
    // Store a full description in the child nodes

    // Down Node
    // == copy description of this node to the down node
    copyFullNode(down_node);
    // == update the branching variable hard bounds for the down node
    // todo(aykut) do we need lower bound for the down node?
    // down_node->vars_->lbHard.entries[branch_var] =
    //   model->getColLower()[branch_var];
    down_node->vars()->ubHard.entries[branch_var] =
      branch_object->ubDownBranch();

    // Up Node
    // == copy description of this node to up node
    copyFullNode(up_node);
    // == update the branching variable hard bounds for the up node
    // todo(aykut) do we need upper bound for the up node?
    up_node->vars()->lbHard.entries[branch_var] =
      branch_object->lbUpBranch();
    // up_node->vars_->ubHard.entries[branch_var] =
    //   model->getColUpper()[branch_var];
  }
  else {
    // Store node description relative to the parent.
    // We need to add a hard bound for the branching variable.

    double ub_down_branch = branch_object->ubDownBranch();
    double lb_up_branch = branch_object->lbUpBranch();
    // todo(aykut) where does colLB and colUB get updated?
    // I think they should stay as they created.
    //double lb = model->colLB()[branch_var];
    //double ub = model->colUB()[branch_var];
    double lb = model->getVariables()[branch_var]->getLbHard();
    double ub = model->getVariables()[branch_var]->getUbHard();
    down_node->setVarHardBound(1,
                             &branch_var,
                             &lb,
                             1,
                             &branch_var,
                             &ub_down_branch);
    up_node->setVarHardBound(1,
                             &branch_var,
                             &lb_up_branch,
                             1,
                             &branch_var,
                             &ub);
  }

  // Down Node
  // == set other relevant fields of down node
  down_node->setBranchedDir(DcoNodeBranchDirectionDown);
  down_node->setBranchedInd(branch_object->index());
  down_node->setBranchedVal(branch_value);
  // == set warm start basis for the down node.
  down_node->setBasis(child_ws);

  // Up Node
  // == set other relevant fields of up node
  up_node->setBranchedDir(DcoNodeBranchDirectionUp);
  up_node->setBranchedInd(branch_object->index());
  up_node->setBranchedVal(branch_value);
  // == set warm start basis for the up node.
  up_node->setBasis(child_ws);

  // Alps does this. We do not need to change the status here
  //status_ = AlpsNodeStatusBranched;

  // push the down and up nodes.
  double quality = model->solver()->getObjValue();
  if (parent_!=NULL) {
    quality = parent_->getQuality();
  }
  res.push_back(CoinMakeTriple(static_cast<AlpsNodeDesc*>(down_node),
                               AlpsNodeStatusCandidate,
                               quality));
  res.push_back(CoinMakeTriple(static_cast<AlpsNodeDesc*>(up_node),
                               AlpsNodeStatusCandidate,
                               quality));
  // grumpy message
  double sum_inf = 0.0;
  for (int i=0; i<model->branchStrategy()->numBranchObjects(); ++i) {
    sum_inf += model->branchStrategy()->branchObjects()[i]->value();
  }
  message_handler->message(DISCO_GRUMPY_MESSAGE_LONG, *messages)
    << broker()->timer().getTime()
    << grumpyMessage[DISCO_GRUMPY_BRANCHED]
    << getIndex()
    << getParentIndex()
    << grumpyDirection[dynamic_cast<DcoNodeDesc*>(desc_)->getBranchedDir()]
    << model->objSense()*getQuality()
    << sum_inf
    << model->branchStrategy()->numBranchObjects()
    << CoinMessageEol;
  // end of grumpy message
  setStatus(AlpsNodeStatusBranched);

  // are these should be in alps level?
  //up_node->setSolEstimate(quality_);
  //down_node->setSolEstimate(quality_);
  return res;
}

// Copies node description of this node to given child node.
// New node is explicitly stored in the memory (no differencing).
void DcoTreeNode::copyFullNode(DcoNodeDesc * child_node) const {
  // get model the node belongs
  DcoModel * model = dynamic_cast<DcoModel*>(broker_->getModel());
  // get message handler and messages for loging
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;
  // get description of this node
  DcoNodeDesc * node_desc = getDesc();

  // solver number of columns
  int num_cols = model->solver()->getNumCols();

  // this will keep bound information, lower and upper
  // it is used for both hard and soft bounds.
  Bound bound;
  bound.lower.ind = new int[num_cols];
  bound.lower.val = new double[num_cols];
  bound.upper.ind = new int[num_cols];
  bound.upper.val = new double[num_cols];

  // Hard bounds
  // == Hard lower bounds
  std::copy(node_desc->getVars()->lbHard.posModify,
            node_desc->getVars()->lbHard.posModify + num_cols,
            bound.lower.ind);
  std::copy(node_desc->getVars()->lbHard.entries,
            node_desc->getVars()->lbHard.entries + num_cols,
            bound.lower.val);
  // == Hard upper bounds
  std::copy(node_desc->getVars()->ubHard.posModify,
            node_desc->getVars()->ubHard.posModify + num_cols,
            bound.upper.ind);
  std::copy(node_desc->getVars()->ubHard.entries,
            node_desc->getVars()->ubHard.entries + num_cols,
            bound.upper.val);

  // assign hard bounds for the child node.
  // this takes ownership of the arrays.
  child_node->assignVarHardBound(num_cols,
                                 bound.lower.ind,
                                 bound.lower.val,
                                 num_cols,
                                 bound.upper.ind,
                                 bound.upper.val);

  // Soft bounds.
  // == Soft lower bounds
  // number of entries modified for soft lower bounds
  int sl_num_modify = node_desc->getVars()->lbSoft.numModify;
  // We can reuse bound here since previos arrays are owned by Bcps.
  bound.lower.ind = new int[sl_num_modify];
  bound.lower.val = new double[sl_num_modify];
  std::copy(node_desc->getVars()->lbSoft.posModify,
            node_desc->getVars()->lbSoft.posModify + sl_num_modify,
            bound.lower.ind);
  std::copy(node_desc->getVars()->lbSoft.entries,
            node_desc->getVars()->lbSoft.entries + sl_num_modify,
            bound.lower.val);

  // == Soft upper bounds
  // number of entries modified for soft upper bounds
  int su_num_modify = node_desc->getVars()->ubSoft.numModify;
  bound.upper.ind = new int[su_num_modify];
  bound.upper.val = new double[su_num_modify];
  std::copy(node_desc->getVars()->ubSoft.posModify,
            node_desc->getVars()->ubSoft.posModify + su_num_modify,
            bound.upper.ind);
  std::copy(node_desc->getVars()->ubSoft.entries,
            node_desc->getVars()->ubSoft.entries + su_num_modify,
            bound.upper.val);

  // Assign soft bounds for the child node.
  child_node->assignVarSoftBound(sl_num_modify,
                                 bound.lower.ind,
                                 bound.lower.val,
                                 su_num_modify,
                                 bound.upper.ind,
                                 bound.upper.val);
}


DcoNodeDesc * DcoTreeNode::getDesc() const {
  return dynamic_cast<DcoNodeDesc*>(AlpsTreeNode::getDesc());
}

void DcoTreeNode::processSetPregnant() {
  // get warm start basis from solver
  // todo(aykut) This does not help much if the underlying solver is an IPM
  // based solver.
  DcoModel * model = dynamic_cast<DcoModel*>(broker()->getModel());
  CoinWarmStartBasis * ws = dynamic_cast<CoinWarmStartBasis*>
    (model->solver()->getWarmStart());
  // store basis in the node desciption.
  getDesc()->setBasis(ws);
  // set status pregnant
  setStatus(AlpsNodeStatusPregnant);

  // grumpy message
  double sum_inf = 0.0;
  for (int i=0; i<model->branchStrategy()->numBranchObjects(); ++i) {
    sum_inf += model->branchStrategy()->branchObjects()[i]->value();
  }
  model->dcoMessageHandler_->message(DISCO_GRUMPY_MESSAGE_LONG, *model->dcoMessages_)
    << broker()->timer().getTime()
    << grumpyMessage[DISCO_GRUMPY_PREGNANT]
    << getIndex()
    << getParentIndex()
    << grumpyDirection[dynamic_cast<DcoNodeDesc*>(desc_)->getBranchedDir()]
    << model->objSense()*getQuality()
    << sum_inf
    << model->branchStrategy()->numBranchObjects()
    << CoinMessageEol;
  // end of grumpy message
}

void DcoTreeNode::branchConstrainOrPrice(BcpsSubproblemStatus subproblem_status,
                                         bool & keepBounding,
                                         bool & branch,
                                         bool & generateConstraints,
                                         bool & generateVariables) {
  DcoModel * model = dynamic_cast<DcoModel*>(broker()->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;

  // check whether the subproblem solved properly, solver status should be
  // infeasible or optimal.
  if (subproblem_status==BcpsSubproblemStatusPrimalInfeasible or
      subproblem_status==BcpsSubproblemStatusDualInfeasible) {

    // debug message
    message_handler->message(0, "Dco", "Subproblem is infeasible."
                             " Status set to fathom",
                             'G', DISCO_DLOG_PROCESS)
      << CoinMessageEol;
    // end of debug message

    // subproblem is infeasible, fathom
    keepBounding = false;
    branch = false;
    generateConstraints = false;
    generateVariables = false;

    // grumpy message
    model->dcoMessageHandler_->message(DISCO_GRUMPY_MESSAGE_SHORT,
                                       *model->dcoMessages_)
      << broker()->timer().getTime()
      << grumpyMessage[DISCO_GRUMPY_INFEASIBLE]
      << getIndex()
      << getParentIndex()
      << grumpyDirection[dynamic_cast<DcoNodeDesc*>(desc_)->getBranchedDir()]
      << CoinMessageEol;
    // end of grumpy message

    setStatus(AlpsNodeStatusFathomed);
    //quality_ = ALPS_OBJ_MAX;
    //solEstimate_ = ALPS_OBJ_MAX;
    return;
  }
  if (subproblem_status!=BcpsSubproblemStatusOptimal) {
    message_handler->message(DISCO_SOLVER_FAILED, *messages)
      << CoinMessageEol;
  }

  // subproblem is solved to optimality. Check feasibility of the solution.
  int numColsInf;
  int numRowsInf;
  DcoSolution * sol = model->feasibleSolution(numColsInf, numRowsInf);
  if (numColsInf) {
    // default strategy is to branch, for now
    keepBounding = false;
    branch = true;
    generateVariables = false;
    generateConstraints = false;
    return;
  }
  else {
    // all columns are feasible
    // check conic feasibility
    // numRowInf will have the number of infeasible conic constraints.
    // since we relax rows corresponding to conic constraints only.
    if (numRowsInf) {
      if (quality_ > broker()->getBestQuality()) {
        // clear stored string
        std::stringstream msg;
        msg << "Subproblem objective value is greater than problem upper bound."
            << " The node is set for fathoming.";
        message_handler->message(0, "Dco", msg.str().c_str(),
                                 'G', DISCO_DLOG_PROCESS)
          << CoinMessageEol;

        // grumpy message
        model->dcoMessageHandler_->message(DISCO_GRUMPY_MESSAGE_MED,
                                           *model->dcoMessages_)
          << broker()->timer().getTime()
          << grumpyMessage[DISCO_GRUMPY_FATHOMED]
          << getIndex()
          << getParentIndex()
          << grumpyDirection[dynamic_cast<DcoNodeDesc*>(desc_)->getBranchedDir()]
          << model->objSense()*getQuality()
          << CoinMessageEol;
        // end of grumpy message

        // set status as fathomed
        setStatus(AlpsNodeStatusFathomed);
        // stop bounding
        keepBounding = false;
        branch = false;
        generateVariables = false;
        generateConstraints = false;
        return;
      }
      else {
        // all cols are feasible, keep bounding and generateConstraints
        keepBounding = true;
        branch = false;
        generateVariables = false;
        generateConstraints = true;
        return;
      }
    }
  }
  if (sol) {
    sol->setDepth(depth_);
    //todo(aykut) solution has an index field I beleive that should be filled
    //in Alps level.
    sol->setIndex(broker()->getNumKnowledges(AlpsKnowledgeTypeSolution));

    model->storeSolution(sol);
    // set node status to fathomed.
    message_handler->message(0, "Dco", "Node is feasible, fathoming... ",
                             'G', DISCO_DLOG_BRANCH)
      << CoinMessageEol;

    // grumpy message
    model->dcoMessageHandler_->message(DISCO_GRUMPY_MESSAGE_MED,
                                       *model->dcoMessages_)
      << broker()->timer().getTime()
      << grumpyMessage[DISCO_GRUMPY_INTEGER]
      << getIndex()
      << getParentIndex()
      << grumpyDirection[dynamic_cast<DcoNodeDesc*>(desc_)->getBranchedDir()]
      << model->objSense()*getQuality()
      << CoinMessageEol;
    // end of grumpy message

    // default strategy is to branch, for now
    keepBounding = false;
    branch = false;
    generateVariables = false;
    generateConstraints = false;

    solEstimate_ = quality_;
    setStatus(AlpsNodeStatusFathomed);
  }
  else {
    message_handler->message(9998, "Dco", "This should not happen.",
                             'E')
      << CoinMessageEol;
  }
}

//todo(aykut) replace this with DcoModel::feasibleSolution????
void DcoTreeNode::checkRelaxedCols(int & numInf) {
  DcoModel * model = dynamic_cast<DcoModel*>(broker()->getModel());
  double const * sol = model->solver()->getColSolution();
  // get integer tolerance
  double tol = model->dcoPar()->entry(DcoParams::integerTol);
  numInf = 0;
  int numRelaxedCols = model->numRelaxedCols();
  int const * relaxedCols = model->relaxedCols();
  //std::vector<BcpsVariable*> & cols = getVariables();
  // iterate over relaxed columns
  for (int i=0; i<numRelaxedCols; ++i) {
    double value = sol[relaxedCols[i]];
    double down = floor(value);
    double up = ceil(value);
    if ((value-down)<tol or (up-value)<tol) {
      continue;
    }
    else {
      numInf++;
    }
  }
}

// todo(aykut) this should go into the con generator. process the cuts given
// by cgl in disco con generator.
void DcoTreeNode::applyConstraints(BcpsConstraintPool const * conPool) {
  DcoModel * model = dynamic_cast<DcoModel*>(broker()->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;
  double scale_par = model->dcoPar()->entry(DcoParams::scaleConFactor);
  double tailoff = model->dcoPar()->entry(DcoParams::tailOff);
  double const * sol = model->solver()->getColSolution();

  // Tranform constraints to Osi cut so that easily add them to LP.
  int num_cuts = conPool->getNumConstraints();
  OsiRowCut const ** cuts_to_add = new OsiRowCut const * [num_cuts];
  int num_add = 0;
  std::vector<int> cuts_to_del;

  // iterate over cuts and
  //------------------------------------------
  // Remove:
  //  - empty cuts
  //  - dense cuts
  //  - bad scaled cuts
  //  - weak cuts
  //  - parallel cuts
  //------------------------------------------
  for (int i=0; i<num_cuts; ++i) {
    DcoLinearConstraint * curr_con =
      dynamic_cast<DcoLinearConstraint*>(conPool->getConstraint(i));
    int length = curr_con->getSize();
    double const * elements = curr_con->getValues();
    int const * indices = curr_con->getIndices();

    // check whether cut is empty.
    if (length <= 0) {
      cuts_to_del.push_back(i);
      continue;
    }
    // check whether cut is dense.
    if(length > 100) {
      // discard the cut
      //cuts_to_del.push_back(i);
      //continue;
    }

    // check cut scaling
    double activity = 0.0;
    double maxElem = 0.0;
    double minElem = ALPS_DBL_MAX;
    double scaleFactor;

    // iterate over coefficients
    for (int k=0; k<length; ++k) {
      if (elements[k]==0.0) {
        // if a coef is exactly 0, ignore it.
        // what if all coef is zero?
        continue;
      }
      if (fabs(elements[k]) > maxElem) {
        maxElem = fabs(elements[k]);
      }
      if (fabs(elements[k]) < minElem) {
        minElem = fabs(elements[k]);
      }
      activity += elements[k] * sol[indices[k]];
    }
    if (maxElem==0.0) {
      // all coefficients are 0, skip cut
      cuts_to_del.push_back(i);
      continue;
    }
    scaleFactor = maxElem/minElem;

    if (scaleFactor > scale_par) {
      // skip the cut since it is badly scaled.
      //cuts_to_del.push_back(i);
      //continue;
    }

    // Check whether the cut is weak.
    double rowLower = CoinMax(curr_con->getLbHard(),
                              curr_con->getLbSoft());
    double rowUpper = CoinMin(curr_con->getUbHard(),
                              curr_con->getUbSoft());
    // violation will be positive if the cut cuts the solution.
    double violation = -1.0;
    if (rowLower > -ALPS_INFINITY) {
      violation = rowLower - activity;
    }
    if (rowUpper < ALPS_INFINITY) {
      violation = CoinMax(violation, activity-rowUpper);
    }

    if (violation < tailoff) {
      // cut is weak, skip it.
      cuts_to_del.push_back(i);
      message_handler->message(0, "Dco", "Cut is ignored since the activity "
                               "is less than tail off value.",
                               'G', DISCO_DLOG_CUT);
      continue;
    }

    // todo(aykut)
    // Check whether cut is parallel to an existing constraint or cut.
    bool parallel = false;
    if (parallel) {
      cuts_to_del.push_back(i);
      continue;
    }
    cuts_to_add[num_add++] = curr_con->createOsiRowCut(model);
  }

  // update statistics of the cut generator


  // Add cuts to lp and adjust basis.
  CoinWarmStartBasis * ws = dynamic_cast<CoinWarmStartBasis*>
    (model->solver()->getWarmStart());

  int num_solver_rows = model->solver()->getNumRows();
  int num_solver_cols = model->solver()->getNumCols();

  if (num_add > 0) {
    model->solver()->applyRowCuts(num_add, cuts_to_add);
    ws->resize(num_solver_rows + num_add, num_solver_cols);
    for (int i=0; i<num_add; ++i) {
      ws->setArtifStatus(num_solver_rows + i,
                         CoinWarmStartBasis::basic);
    }
    if (model->solver()->setWarmStart(ws) == false) {
      throw CoinError("Fail setWarmStart() after cut installation.",
                      "applyConstraints","DcoTreeNode");
    }
    for (int k=0; k<num_add; ++k) {
      delete cuts_to_add[k];
    }
  }

  // debug stuff
  std::stringstream debug_msg;
  debug_msg << num_add;
  debug_msg << " out of ";
  debug_msg << conPool->getNumConstraints();
  debug_msg << "  cuts added to the solver.";
  message_handler->message(0, "Dco", debug_msg.str().c_str(),
                           'G', DISCO_DLOG_CUT)
    << CoinMessageEol;
  // end of debug stuff


  delete[] cuts_to_add;
  delete ws;
}

/// Pack this into an encoded object.
AlpsReturnStatus DcoTreeNode::encode(AlpsEncoded * encoded) const {
  // get pointers for message logging
  assert(broker_);
  DcoModel * model = dynamic_cast<DcoModel*>(broker_->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;

  // return value
  AlpsReturnStatus status;
  status = AlpsTreeNode::encode(encoded);
  assert(status==AlpsReturnStatusOk);
  status = BcpsTreeNode::encode(encoded);
  assert(status==AlpsReturnStatusOk);

  // debug stuff
  std::stringstream debug_msg;
  debug_msg << "Proc[" << broker_->getProcRank() << "]"
            << " node " << this << " encoded." << std::endl;
  message_handler->message(0, "Dco", debug_msg.str().c_str(),
                              'G', DISCO_DLOG_MPI)
    << CoinMessageEol;
  // end of debug stuff

  return status;
}

/// Unpack into a new DcoTreeNode object and return a pointer to it.
AlpsKnowledge * DcoTreeNode::decode(AlpsEncoded & encoded) const {
  // return value
  AlpsReturnStatus status;
  // todo(aykut) we are decoing, how do we know the model to assign the new
  // node is same as the model_ of this? Is this due to fact that they are in
  // the same processor?
  AlpsNodeDesc * new_node_desc = new DcoNodeDesc();
  DcoTreeNode * new_node = new DcoTreeNode(new_node_desc);
  new_node->setBroker(broker_);
  new_node_desc = NULL;
  status = new_node->decodeToSelf(encoded);
  assert(status==AlpsReturnStatusOk);
  return new_node;
}

/// Unpack into this from an encoded object.
AlpsReturnStatus DcoTreeNode::decodeToSelf(AlpsEncoded & encoded) {
  // get pointers related to message logging.
  assert(broker_);
  DcoModel * model = dynamic_cast<DcoModel*>(broker_->getModel());
  CoinMessageHandler * message_handler = model->dcoMessageHandler_;
  CoinMessages * messages = model->dcoMessages_;

  // return value
  AlpsReturnStatus status;
  status = AlpsTreeNode::decodeToSelf(encoded);
  assert(status==AlpsReturnStatusOk);
  status = BcpsTreeNode::decodeToSelf(encoded);
  assert(status==AlpsReturnStatusOk);

  // debug stuff
  std::stringstream debug_msg;
  debug_msg << "Proc[" << broker_->getProcRank() << "]"
            << " node decoded into " << this << "." << std::endl;
  message_handler->message(0, "Dco", debug_msg.str().c_str(),
                              'G', DISCO_DLOG_MPI)
    << CoinMessageEol;
  // end of debug stuff

  return status;
}
