// [[Rcpp::depends(RcppArmadillo)]]
#include "stemr_types.h"
#include "stemr_utils.h"

using namespace Rcpp;
using namespace arma;

//' Simulate an LNA path using a non-centered parameterization for the
//' log-transformed counting process LNA.
//'
//' @param lna_times vector of interval endpoint times
//' @param lna_pars numeric matrix of parameters, constants, and time-varying
//'   covariates at each of the lna_times
//' @param init_start index in the parameter vector where the initial compartment
//'   volumes start
//' @param param_update_inds logical vector indicating at which of the times the
//'   LNA parameters need to be updated.
//' @param stoich_matrix stoichiometry matrix giving the changes to compartments
//'   from each reaction
//' @param lna_pointer external pointer to LNA integration function.
//' @param set_pars_pointer external pointer to the function for setting the LNA
//'   parameters.
//'
//' @return list containing the stochastic perturbations (i.i.d. N(0,1) draws) and
//' the LNA path on its natural scale which is determined by the perturbations.
//'
//' @export
// [[Rcpp::export]]
Rcpp::List propose_lna(const arma::rowvec& lna_times, const Rcpp::NumericMatrix& lna_pars,
                       const int init_start, const Rcpp::LogicalVector& param_update_inds,
                       const arma::mat& stoich_matrix, SEXP lna_pointer, SEXP set_pars_pointer) {

        // get the dimensions of various objects
        int n_events = stoich_matrix.n_cols;         // number of transition events, e.g., S2I, I2R
        int n_comps  = stoich_matrix.n_rows;         // number of model compartments (all strata)
        int n_odes   = n_events + n_events*n_events; // number of ODEs
        int n_times  = lna_times.n_elem;             // number of times at which the LNA must be evaluated
        int init_end = init_start + n_comps;         // index in the parameter vector to stop copying

        // initialize the objects used in each time interval
        double t_L = 0;
        double t_R = 0;
        Rcpp::NumericVector current_params = lna_pars.row(0);   // vector for storing the current parameter values
        CALL_SET_LNA_PARAMS(current_params, set_pars_pointer);  // set the parameters in the odeintr namespace

        // initial state vector - copy elements from the current parameter vector
        arma::vec init_state(current_params.begin() + init_start, n_comps);
        arma::vec init_volumes(init_state.begin(), n_comps);

        // initialize the LNA objects - the vector for storing the current state
        Rcpp::NumericVector lna_state_vec(n_odes);   // vector to store the results of the ODEs

        arma::vec lna_drift(n_events, arma::fill::zeros); // incidence mean vector (natural scale)
        arma::mat lna_diffusion(n_events, n_events, arma::fill::zeros); // diffusion matrix

        arma::vec log_lna(n_events, arma::fill::zeros);  // LNA increment, log scale
        arma::vec nat_lna(n_events, arma::fill::zeros);  // LNA increment, natural scale
        arma::vec c_incid(n_events, arma::fill::zeros);  // cumulative incidence

        // matrix in which to store the LNA path
        arma::mat lna_path(n_events+1, n_times, arma::fill::zeros);
        lna_path.row(0) = lna_times;

        // indices at which the diffusion elements of lna_state vec start
        int diff_start = n_events;

        // sample the stochastic perturbations
        arma::mat draws(n_events, n_times-1, arma::fill::randn);

        // iterate over the time sequence, solving the LNA over each interval
        for(int j=0; j < (n_times-1); ++j) {

                // set the times of the interval endpoints
                t_L = lna_times[j];
                t_R = lna_times[j+1];

                // Reset the LNA state vector and integrate the LNA ODEs over the next interval to 0
                std::fill(lna_state_vec.begin(), lna_state_vec.end(), 0.0);
                CALL_INTEGRATE_STEM_LNA(lna_state_vec, t_L, t_R, 0.001, lna_pointer);

                // transfer the elements of the lna_state_vec to the process objects
                std::copy(lna_state_vec.begin(), lna_state_vec.begin() + n_events, lna_drift.begin());
                std::copy(lna_state_vec.begin() + n_events, lna_state_vec.end(), lna_diffusion.begin());

                // ensure symmetry of the diffusion matrix
                lna_diffusion = arma::symmatu(lna_diffusion);

                // map the stochastic perturbation to the LNA path on its natural scale
                try{
                        log_lna = lna_drift + arma::chol(lna_diffusion, "lower") * draws.col(j);
                } catch(std::runtime_error &err) {
                        forward_exception_to_r(err);
                }

                // compute the LNA increment and clamp below by 0
                nat_lna = arma::exp(log_lna) - 1;
                nat_lna.elem(arma::find(nat_lna < 0)).zeros();

                // update the cumulative incidence
                c_incid += nat_lna;
                lna_path(arma::span(1,n_events), j+1) = c_incid;

                // update the compartment volumes
                init_volumes = init_state + stoich_matrix * c_incid;
                init_volumes.elem(arma::find(init_volumes < 0)).zeros();

                // update the parameters if they need to be updated
                if(param_update_inds[j+1]) {
                        current_params = lna_pars.row(j+1);
                }

                // copy the compartment volumes to the current parameters
                std::copy(init_volumes.begin(), init_volumes.end(), current_params.begin() + init_start);

                // set the lna parameters and reset the LNA state vector
                CALL_SET_LNA_PARAMS(current_params, set_pars_pointer);
        }

        // return the paths
        return Rcpp::List::create(Rcpp::Named("draws")     = draws,
                                  Rcpp::Named("lna_path")  = lna_path.t());
}