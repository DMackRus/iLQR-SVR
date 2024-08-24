#include "StdInclude.h"
#include "FileHandler.h"
#include "Visualiser.h"
#include "MuJoCoHelper.h"

// --------------------- different scenes -----------------------
#include "ModelTranslator/TwoDPushing.h"
#include "ModelTranslator/PushSoft.h"

// --------------------- different optimisers -----------------------
//#include "Optimiser/iLQR.h"
#include "iLQR_SVR.h"

//----------------------- Testing methods ---------------------------
#include "GenTestingData.h"

// --------------------- other -----------------------
#include <mutex>

// --------------------- Global class instances --------------------------------
std::shared_ptr<ModelTranslator> activeModelTranslator;
std::shared_ptr<Differentiator> activeDifferentiator;
std::shared_ptr<iLQR_SVR> iLQR_SVR_Optimiser;
std::shared_ptr<Visualiser> activeVisualiser;
std::shared_ptr<FileHandler> yamlReader;

bool record_trajectory = false;
bool async_mpc = true;
std::string task;
bool mpc_visualise = true;
bool playback = true;

std::mutex mtx;
std::condition_variable cv;

std::vector<stateVectorList> tracking_state_vector;
stateVectorList current_mpc_state_vector;

bool apply_next_control = false;

int assign_task();

void InitControls();
void OpenLoopOptimisation(int opt_horizon);
void MPCUntilComplete(int OPT_HORIZON);

void AsyncMPC();
void worker();

void change_cost_func_push_soft();

double avg_opt_time, avg_percent_derivs, avg_time_derivs, avg_time_bp, avg_time_fp;

bool stop_mpc = false;

int mpc_num_controls_apply = 80;
int num_steps_replan = 1;

volatile bool reoptimise = false;

int main(int argc, char **argv) {

    // Minimum Expected arguments
    // 1. Program name
    // 2. Config file name
    if(argc < 2){
        std::cout << "No task name provided, exiting" << endl;
        return -1;
    }

    std::string config_file_name = argv[1];

    std::string optimiser;
    std::string runMode;

    std::string taskInitMode;

    yamlReader = std::make_shared<FileHandler>();
    yamlReader->ReadSettingsFile("/GeneralConfigs/" + config_file_name + ".yaml");
    runMode = yamlReader->project_run_mode;
    task = yamlReader->taskName;
    taskInitMode = yamlReader->taskInitMode;
    async_mpc = yamlReader->async_mpc;
    record_trajectory = yamlReader->record_trajectory;

    // Instantiate model translator as specified by the config file.
    if(assign_task() == EXIT_FAILURE){
        return EXIT_FAILURE;
    }

    activeDifferentiator = std::make_shared<Differentiator>(activeModelTranslator, activeModelTranslator->MuJoCo_helper);

    for(int j = 0; j < 5; j++){
        mj_step(activeModelTranslator->MuJoCo_helper->model, activeModelTranslator->MuJoCo_helper->master_reset_data);
    }
    activeModelTranslator->MuJoCo_helper->AppendSystemStateToEnd(activeModelTranslator->MuJoCo_helper->master_reset_data);

    //Instantiate my visualiser
    activeVisualiser = std::make_shared<Visualiser>(activeModelTranslator);

    // Setup the initial horizon, based on open loop or mpc method
    int opt_horizon = 0;
    if(runMode == "MPC_Until_Complete"){
        opt_horizon = activeModelTranslator->MPC_horizon;
    }
    else{
        opt_horizon = activeModelTranslator->openloop_horizon;
    }

    // Setup the optimiser
    iLQR_SVR_Optimiser = std::make_shared<iLQR_SVR>(activeModelTranslator,
                                                    activeModelTranslator->MuJoCo_helper,
                                                    activeDifferentiator,
                                                    opt_horizon, activeVisualiser, yamlReader);


//    if(runMode == "Generate_test_scenes"){
//        GenTestingData myTestingObject(iLQROptimiser, activeModelTranslator,
//                                       activeDifferentiator, activeVisualiser, yamlReader);
//
//        return myTestingObject.GenerateTestScenes(100);
//    }

//    if(runMode == "Generate_openloop_data"){
//        GenTestingData myTestingObject(activeOptimiser, activeModelTranslator,
//                                       activeDifferentiator, activeVisualiser, yamlReader);
//
//        int task_horizon = activeModelTranslator->openloop_horizon;
//
//        // Optimisation horizon
//        if(argc > 2){
//            task_horizon = std::atoi(argv[2]);
//        }
//
//        if(optimiser == "iLQR_SVR"){
//            if(argc > 3){
//                std::cout << "setting optimiser parameters \n";
//                int re_add_dofs = std::atoi(argv[3]);
//                int K_threshold = std::atof(argv[4]);
//
//                myTestingObject.SetParamsiLQR_SVR(re_add_dofs, K_threshold);
//            }
//        }
//
//        return myTestingObject.GenDataOpenLoopMultipleMethods(task_horizon);
//    }
//    if(runMode == "Generate_asynchronus_mpc_data"){
//        GenTestingData myTestingObject(activeOptimiser, activeModelTranslator,
//                                       activeDifferentiator, activeVisualiser, yamlReader);
//
//        int task_horizon = 60;
//        int task_timeout = 2000;
//        int re_add_dofs;
//        double K_threshold;
//
//        if(argc > 2){
//            task_horizon = std::atoi(argv[2]);
//        }
//
//        if(argc > 3){
//            task_timeout = std::atoi(argv[3]);
//        }
//
//        if(argc > 4){
//            re_add_dofs = std::atoi(argv[4]);
//            K_threshold = std::atof(argv[5]);
//
//            myTestingObject.SetParamsiLQR_SVR(re_add_dofs, K_threshold);
//            std::cout << "set optimiser parameters \n";
//        }
//
//        return myTestingObject.GenDataAsyncMPC(task_horizon, task_timeout);
//    }

    if(taskInitMode == "random"){
        activeModelTranslator->GenerateRandomGoalAndStartState();
    }
    else if(taskInitMode == "fromCSV") {
        std::string task_prefix = activeModelTranslator->model_name;
        yamlReader->LoadTaskFromFile(task_prefix, yamlReader->csvRow, activeModelTranslator->full_state_vector,
                                     activeModelTranslator->residual_list);
        activeModelTranslator->full_state_vector.Update();
        activeModelTranslator->current_state_vector = activeModelTranslator->full_state_vector;
        activeModelTranslator->UpdateSceneVisualisation();
    }

    // Initialise the system state from full state vector here
    activeModelTranslator->InitialiseSystemToStartState(activeModelTranslator->MuJoCo_helper->master_reset_data);

    // Methods of control / visualisation
    if(runMode == "Init_controls"){
        cout << "SHOWING INIT CONTROLS MODE \n";
        InitControls();
    }
    else if(runMode == "Optimise_once"){
        cout << "OPTIMISE TRAJECTORY ONCE AND DISPLAY MODE \n";
        iLQR_SVR_Optimiser->verbose_output = true;
        OpenLoopOptimisation(activeModelTranslator->openloop_horizon);
    }
    else if(runMode == "MPC_until_completion"){
        cout << "MPC UNTIL TASK COMPLETE MODE \n";
        AsyncMPC();
    }
    else{
        cout << "INVALID MODE OF OPERATION OF PROGRAM \n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

void InitControls(){
    int setupHorizon = 1000;
    int optHorizon = 2500;
    int controlCounter = 0;
    int visualCounter = 0;

    std::vector<MatrixXd> initControls;

    std::vector<MatrixXd> initSetupControls = activeModelTranslator->CreateInitSetupControls(setupHorizon);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->master_reset_data, activeModelTranslator->MuJoCo_helper->main_data);
    std::vector<MatrixXd> initOptimisationControls = activeModelTranslator->CreateInitOptimisationControls(optHorizon);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->main_data, activeModelTranslator->MuJoCo_helper->master_reset_data);

    //Stitch setup and optimisation controls together
    initControls.insert(initControls.end(), initOptimisationControls.begin(), initOptimisationControls.end());


    if(record_trajectory){
        activeVisualiser->StartRecording(task + "_init_controls");
    }

    while(activeVisualiser->windowOpen()){

        activeModelTranslator->SetControlVector(initControls[controlCounter], activeModelTranslator->MuJoCo_helper->main_data,
                                                activeModelTranslator->current_state_vector);
        mj_step(activeModelTranslator->MuJoCo_helper->model, activeModelTranslator->MuJoCo_helper->main_data);


        controlCounter++;
        visualCounter++;

        if(controlCounter >= initControls.size()){
            controlCounter = 0;
            activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->main_data, activeModelTranslator->MuJoCo_helper->master_reset_data);
            activeVisualiser->StopRecording();
        }

        if(visualCounter > 5){
            visualCounter = 0;
            activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->vis_data, activeModelTranslator->MuJoCo_helper->main_data);
            activeModelTranslator->MuJoCo_helper->ForwardSimulator(activeModelTranslator->MuJoCo_helper->vis_data);
            if(record_trajectory){
                activeVisualiser->render("");
            }
            else{
                activeVisualiser->render("show init controls");
            }
        }
    }
}

void OpenLoopOptimisation(int opt_horizon){
    int control_counter = 0;
    int visual_counter = 0;
    bool show_opt_controls = true;
    const char* label = "Final trajectory after optimisation";

    std::vector<MatrixXd> init_controls;
    std::vector<MatrixXd> optimised_controls;

    std::vector<MatrixXd> init_setup_controls = activeModelTranslator->CreateInitSetupControls(1000);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->master_reset_data, activeModelTranslator->MuJoCo_helper->main_data);

    std::vector<MatrixXd> init_opt_controls = activeModelTranslator->CreateInitOptimisationControls(opt_horizon);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->main_data, activeModelTranslator->MuJoCo_helper->master_reset_data);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->saved_systems_state_list[0], activeModelTranslator->MuJoCo_helper->master_reset_data);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->vis_data, activeModelTranslator->MuJoCo_helper->master_reset_data);

    std::vector<MatrixXd> optimisedControls = iLQR_SVR_Optimiser->Optimise(activeModelTranslator->MuJoCo_helper->saved_systems_state_list[0],
                                                                        init_opt_controls, yamlReader->maxIter,
                                                                        yamlReader->minIter, opt_horizon);

    // Stitch together setup controls with init control + optimised controls
    init_controls.insert(init_controls.end(), init_opt_controls.begin(), init_opt_controls.end());
    optimised_controls.insert(optimised_controls.end(), optimisedControls.begin(), optimisedControls.end());

    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->main_data, activeModelTranslator->MuJoCo_helper->master_reset_data);

    if(record_trajectory){
        activeVisualiser->StartRecording(task + "_optimised_controls");
        label = "";
    }
    while(activeVisualiser->windowOpen()){

        if(show_opt_controls){
            activeModelTranslator->SetControlVector(optimised_controls[control_counter], activeModelTranslator->MuJoCo_helper->main_data,
                                                    activeModelTranslator->current_state_vector);
        }
        else{
            activeModelTranslator->SetControlVector(init_controls[control_counter], activeModelTranslator->MuJoCo_helper->main_data,
                                                    activeModelTranslator->current_state_vector);
        }

        mj_step(activeModelTranslator->MuJoCo_helper->model, activeModelTranslator->MuJoCo_helper->main_data);

        control_counter++;
        visual_counter++;

        if(control_counter >= optimised_controls.size()){
            control_counter = 0;
            activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->main_data, activeModelTranslator->MuJoCo_helper->master_reset_data);
            show_opt_controls = !show_opt_controls;
            if(show_opt_controls){
                label = "Final trajectory after optimisation";
            }
            else{
                label = "Initial trajectory before optimisation";
            }

            activeVisualiser->StopRecording();
        }

        if(visual_counter >= 5){
            visual_counter = 0;
            activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->vis_data, activeModelTranslator->MuJoCo_helper->main_data);
            activeModelTranslator->MuJoCo_helper->ForwardSimulator(activeModelTranslator->MuJoCo_helper->vis_data);
            activeVisualiser->render(label);
        }
    }
}

void worker(){
    MPCUntilComplete(activeModelTranslator->MPC_horizon);
}

void AsyncMPC(){

    // Setup the task
    std::vector<MatrixXd> initSetupControls = activeModelTranslator->CreateInitSetupControls(1000);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->master_reset_data, activeModelTranslator->MuJoCo_helper->main_data);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->vis_data, activeModelTranslator->MuJoCo_helper->master_reset_data);


    // Whether Optimiser will output useful information
    iLQR_SVR_Optimiser->verbose_output = true;
    // Visualise MPC trajectory live
    mpc_visualise = true;
    reoptimise = true;

    // Start the optimisation thread
    std::thread MPC_controls_thread;
    MPC_controls_thread = std::thread(&worker);

    int vis_counter = 0;
    MatrixXd next_control;

    // timer variables
    std::chrono::steady_clock::time_point begin;
    std::chrono::steady_clock::time_point end;

    int MAX_TASK_TIME = 2000;
    int task_time = 0;

    // Setup initial state vector for visualisation
    current_mpc_state_vector = activeModelTranslator->current_state_vector;

    std::vector<MatrixXd> replay_states;
    int nq = activeModelTranslator->MuJoCo_helper->model->nq;
    int nv = activeModelTranslator->MuJoCo_helper->model->nv;
    MatrixXd full_state(nq + nv, 1);
    auto *_full_state = new double[nq+nv];

    while(task_time < MAX_TASK_TIME){
        begin = std::chrono::steady_clock::now();

        if(async_mpc || (!async_mpc && apply_next_control)){

            tracking_state_vector.push_back(current_mpc_state_vector);

            if(!async_mpc){
                apply_next_control = false;
            }

            // if correct number of controls applies, trigger reoptimise

            // if control buffer exhausted, apply
            if(activeVisualiser->current_control_index >= num_steps_replan){
                // Applied correct number of controls so lets replan
                reoptimise = true;
            }

            // if control index > numapplycontrols
            if(activeVisualiser->current_control_index < mpc_num_controls_apply && activeVisualiser->current_control_index < activeVisualiser->controlBuffer.size()){
//            if(activeVisualiser->current_control_index < activeVisualiser->controlBuffer.size()){

                next_control = activeVisualiser->controlBuffer[activeVisualiser->current_control_index];
                // Increment the current control index
                activeVisualiser->current_control_index++;

                double controls_noise_percentage = 5;
                MatrixXd control_lims = activeModelTranslator->ReturnControlLimits(activeModelTranslator->current_state_vector);
                for(int i = 0; i < activeModelTranslator->current_state_vector.num_ctrl; i++){
                    double control_noise = ((control_lims(i*2 + 1) - control_lims(i*2)) / 100) * controls_noise_percentage;

                    double gauss_noise = GaussNoise(0, control_noise);
                    next_control(i, 0) += gauss_noise;
                }
            }
            else{
                std::vector<double> grav_compensation;
                std::string robot_name = activeModelTranslator->current_state_vector.robots[0].name;
                activeModelTranslator->MuJoCo_helper->GetRobotJointsGravityCompensationControls(robot_name, grav_compensation,
                                                                                                activeModelTranslator->MuJoCo_helper->vis_data);
                MatrixXd empty_control(activeModelTranslator->current_state_vector.num_ctrl, 1);
//                empty_control.setZero();
                for(int i = 0; i < activeModelTranslator->current_state_vector.num_ctrl; i++){
                    empty_control(i) = grav_compensation[i];
                }
                next_control = empty_control;
            }

//            std::cout << "next control is: " << next_control.transpose() << "\n";

            // Store latest control and state in a replay buffer
            activeVisualiser->trajectory_controls.push_back(next_control);

            // We store the full state for visualisation relay purposes
            mj_getState(activeModelTranslator->MuJoCo_helper->model,
                        activeModelTranslator->MuJoCo_helper->vis_data, _full_state, mjSTATE_PHYSICS);
            for(int i = 0; i < nq+nv; i++){
                full_state(i, 0) = _full_state[i];
            }
            replay_states.push_back(full_state);
            // ----------------------------------------

            MatrixXd next_state = activeModelTranslator->ReturnStateVectorQuaternions(activeModelTranslator->MuJoCo_helper->vis_data, activeModelTranslator->full_state_vector);
            activeVisualiser->trajectory_states.push_back(next_state);

            // Set the latest control
            activeModelTranslator->SetControlVector(next_control, activeModelTranslator->MuJoCo_helper->vis_data,
                                                    activeModelTranslator->current_state_vector);

            // Update the simulation
            mj_step(activeModelTranslator->MuJoCo_helper->model, activeModelTranslator->MuJoCo_helper->vis_data);
            task_time++;

            // Check if task complete
            double dist;
            if(activeModelTranslator->TaskComplete(activeModelTranslator->MuJoCo_helper->vis_data, dist)){
                cout << "task complete - dist: " << dist  << endl;
                break;
            }
        }

        // Update the visualisation
        // Unsure why rendering every time causes it to lag so much more???
        vis_counter++;
        if(vis_counter > 5){
            activeVisualiser->render("live-MPC");
            vis_counter = 0;
        }

        end = std::chrono::steady_clock::now();
        // time taken
        auto time_taken = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();

        // compare how long we took versus the timestep of the model, also accoutning for desired lowdown factor
        int difference_ms = (activeModelTranslator->MuJoCo_helper->ReturnModelTimeStep() * 1000 * activeModelTranslator->slowdown_factor)
                                - (time_taken / 1000.0f) + 1;

        if(difference_ms > 0) {
//            difference_ms += 20;
            std::this_thread::sleep_for(std::chrono::milliseconds(difference_ms));
        }
    }

    // Stop MPC thread
    {
        std::unique_lock<std::mutex> lock(mtx);
        stop_mpc = true;
    }

    MPC_controls_thread.join();

    double cost = 0.0f;
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->vis_data, activeModelTranslator->MuJoCo_helper->master_reset_data);

    bool terminal = false;

//    if(task == "pushing_moderate_clutter"){
//        for(auto& rigid_body : activeModelTranslator->full_state_vector.rigid_bodies){
//            rigid_body.terminal_linear_pos_cost[0] = 10000;
//            rigid_body.terminal_linear_pos_cost[1] = 10000;
//        }
//    }

    if(record_trajectory){
        activeVisualiser->StartRecording(task + "_MPC");
    }
    for(int i = 0; i < activeVisualiser->trajectory_states.size(); i++){

        // Update current state vector for visualising what was being consider at this point
        activeModelTranslator->current_state_vector = tracking_state_vector[i];
        activeModelTranslator->UpdateSceneVisualisation();

        activeModelTranslator->SetControlVector(activeVisualiser->trajectory_controls[i], activeModelTranslator->MuJoCo_helper->vis_data,
                                                activeModelTranslator->full_state_vector);

        // - complicated but works well, we use full mj_state for replay as we want to visualise all dofs and how they change
        // - not just the ones in the state vector
        for(int j = 0; j < nq+nv; j++){
            _full_state[j] = replay_states[i](j, 0);
        }
        mj_setState(activeModelTranslator->MuJoCo_helper->model,
                    activeModelTranslator->MuJoCo_helper->vis_data, _full_state, mjSTATE_PHYSICS);
        activeModelTranslator->MuJoCo_helper->ForwardSimulator(activeModelTranslator->MuJoCo_helper->vis_data);
        if(i == activeVisualiser->trajectory_states.size() - 1){
            terminal = true;
        }

        MatrixXd residuals(activeModelTranslator->residual_list.size(), 1);
        activeModelTranslator->Residuals(activeModelTranslator->MuJoCo_helper->vis_data, residuals);
        cost += activeModelTranslator->CostFunction(residuals, activeModelTranslator->full_state_vector, terminal);
        if(i % 5 == 0){
            activeVisualiser->render("");
        }
    }
    activeVisualiser->StopRecording();

    delete [] _full_state;

    std::cout << "final cost of entire MPC trajectory was: " << cost << "\n";
    std::cout << "avg opt time: " << avg_opt_time << " ms \n";
    std::cout << "avg percent derivs: " << avg_percent_derivs << " % \n";
    std::cout << "avg time derivs: " << avg_time_derivs << " ms \n";
    std::cout << "avg time BP: " << avg_time_bp << " ms \n";
    std::cout << "avg time FP: " << avg_time_fp << " ms \n";
}

// Before calling this function, we should setup the activeModelTranslator with the correct initial state and the
// Optimiser settings. This function can then return necessary testing data for us to store
void MPCUntilComplete(int OPT_HORIZON){

    std::vector<double> time_get_derivs;
    std::vector<double> time_bp;
    std::vector<double> time_fp;
    std::vector<double> percent_derivs_computed;

    std::vector<MatrixXd> optimised_controls;

    // Instantiate init controls
    std::vector<MatrixXd> init_opt_controls;

    // Create init optimisation controls and reset system state
    optimised_controls = activeModelTranslator->CreateInitOptimisationControls(OPT_HORIZON);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->main_data, activeModelTranslator->MuJoCo_helper->master_reset_data);
    activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->saved_systems_state_list[0], activeModelTranslator->MuJoCo_helper->master_reset_data);

//    optimised_controls = activeOptimiser->Optimise(activeModelTranslator->MuJoCo_helper->saved_systems_state_list[0], init_opt_controls, 1, 1, OPT_HORIZON);
    current_mpc_state_vector = activeModelTranslator->current_state_vector;

    MatrixXd current_state;

    while(!stop_mpc){

        if(reoptimise){
            std::cout << "reoptimise called \n";
            // Copy current state of system (vis data) to starting data object for optimisation
            activeModelTranslator->MuJoCo_helper->CopySystemState(activeModelTranslator->MuJoCo_helper->saved_systems_state_list[0], activeModelTranslator->MuJoCo_helper->vis_data);

            // Get the current control index
            int current_control_index = activeVisualiser->current_control_index;

            // Delete all controls before control index
            optimised_controls.erase(optimised_controls.begin(), optimised_controls.begin() + current_control_index);

            // Copy last control to keep control trajectory the same size
            MatrixXd last_control = optimised_controls.back();
            for (int i = 0; i < current_control_index; ++i) {
                optimised_controls.push_back(last_control);
            }

            optimised_controls = iLQR_SVR_Optimiser->Optimise(activeModelTranslator->MuJoCo_helper->saved_systems_state_list[0], optimised_controls, 1, 1, OPT_HORIZON);
            current_mpc_state_vector = activeModelTranslator->current_state_vector;

            // Store last iteration timing results
            time_get_derivs.push_back(iLQR_SVR_Optimiser->avg_time_get_derivs_ms);
            time_bp.push_back(iLQR_SVR_Optimiser->avg_time_backwards_pass_ms);
            time_fp.push_back(iLQR_SVR_Optimiser->avg_time_forwards_pass_ms);

            int optTimeToTimeSteps = iLQR_SVR_Optimiser->opt_time_ms / (activeModelTranslator->MuJoCo_helper->ReturnModelTimeStep() * 1000);

            // By the time we have computed optimal controls, main visualisation will be some number
            // of time-steps ahead. We need to find the correct control to apply.
            current_state = activeModelTranslator->ReturnStateVector(activeModelTranslator->MuJoCo_helper->vis_data,
                                                                     activeModelTranslator->current_state_vector);

            // Compute the best starting state
            double smallestError = 1000.00;
            int bestMatchingStateIndex = optTimeToTimeSteps;

            if(bestMatchingStateIndex >= OPT_HORIZON){
                bestMatchingStateIndex = OPT_HORIZON - 1;
            }
            for(int i = 0; i < OPT_HORIZON - 1; i++){
//                std::cout << "i: " << i << " state: " << activeOptimiser->X_old[i].transpose() << std::endl;
//                std::cout << "correct state: " << current_vis_state.transpose() << std::endl;
                double currError = 0.0f;
                for(int j = 0; j < activeModelTranslator->current_state_vector.dof*2; j++){
                    // TODO - im not sure about this, should we use full state?
                    currError += abs(iLQR_SVR_Optimiser->X_old[i](j) - current_state(j));
                }
                if(currError < smallestError){
                    smallestError = currError;
                    bestMatchingStateIndex = i;
                }
            }
            bestMatchingStateIndex = 1;

            // Mutex lock
            {
                std::unique_lock<std::mutex> lock(mtx);

                activeVisualiser->controlBuffer = optimised_controls;

                // Set the current control index to the best matching state index
                activeVisualiser->current_control_index = bestMatchingStateIndex;

                apply_next_control = true;
                reoptimise = false;
            }

            std::cout << "best matching state index: " << bestMatchingStateIndex << std::endl;
        }
    }

    avg_time_derivs = 0.0;
    avg_time_bp = 0.0;
    avg_time_fp = 0.0;
    avg_percent_derivs = 0.0;

    for(int i = 0; i < time_get_derivs.size(); i++){
        avg_time_derivs += time_get_derivs[i];
        avg_time_bp += time_bp[i];
        avg_time_fp += time_fp[i];
        avg_percent_derivs += percent_derivs_computed[i];
    }

    avg_time_derivs /= time_get_derivs.size();
    avg_time_bp /= time_bp.size();
    avg_time_fp /= time_fp.size();
    avg_percent_derivs /= percent_derivs_computed.size();

    avg_opt_time = avg_time_derivs + avg_time_bp + avg_time_fp;
}

int assign_task(){
    if(task == "pushing_moderate_clutter"){
        std::shared_ptr<TwoDPushing> myTwoDPushing = std::make_shared<TwoDPushing>(yamlReader,heavyClutter);
        activeModelTranslator = myTwoDPushing;
    }
    else if(task == "push_soft_into_rigid"){
        std::shared_ptr<PushSoft> my_squish_soft = std::make_shared<PushSoft>(yamlReader, PUSH_SOFT_RIGID);
        activeModelTranslator = my_squish_soft;
    }
    else if(task == "push_soft"){
        std::shared_ptr<PushSoft> my_squish_soft = std::make_shared<PushSoft>(yamlReader, PUSH_SOFT);
        activeModelTranslator = my_squish_soft;
    }
    else{
        std::cout << "invalid scene selected, " << task << " does not exist" << std::endl;
    }
    return EXIT_SUCCESS;
}

void change_cost_func_push_soft(){
//    for(int i = 0; i < activeModelTranslator->full_state_vector.soft_bodies[0].num_vertices; i++){
//        activeModelTranslator->full_state_vector.soft_bodies[0].linearPosCost[0] = 1;
//        activeModelTranslator->full_state_vector.soft_bodies[0].linearPosCost[0] = 1;
//    }
}