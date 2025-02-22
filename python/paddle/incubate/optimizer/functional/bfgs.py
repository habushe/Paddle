# Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import numpy as np

from .line_search import strong_wolfe
from .utils import _value_and_gradient, check_input_type, check_initial_inverse_hessian_estimate

import paddle


def minimize_bfgs(objective_func,
                  initial_position,
                  max_iters=50,
                  tolerance_grad=1e-7,
                  tolerance_change=1e-9,
                  initial_inverse_hessian_estimate=None,
                  line_search_fn='strong_wolfe',
                  max_line_search_iters=50,
                  initial_step_length=1.0,
                  dtype='float32',
                  name=None):
    r"""
    Minimizes a differentiable function `func` using the BFGS method.
    The BFGS is a quasi-Newton method for solving an unconstrained
    optimization problem over a differentiable function.
    Closely related is the Newton method for minimization. Consider the iterate 
    update formula
    .. math::
        x_{k+1} = x_{k} + H \nabla{f},
    If $H$ is the inverse Hessian of $f$ at $x_{k}$, then it's the Newton method.
    If $H$ is symmetric and positive definite, used as an approximation of the inverse Hessian, then 
    it's a quasi-Newton. In practice, the approximated Hessians are obtained
    by only using the gradients, over either whole or part of the search 
    history, the former is BFGS.

    Reference:
        Jorge Nocedal, Stephen J. Wright, Numerical Optimization, Second Edition, 2006.
        pp140: Algorithm 6.1 (BFGS Method).
    
    Following summarizes the the main logic of the program based on BFGS. Note: _k represents value of
    k_th iteration, ^T represents the transposition of a vector or matrix.
    repeat
        p_k = H_k * g_k
        alpha = strong_wolfe(f, x_k, p_k)
        x_k+1 = x_k + alpha * p_k
        s_k = x_k+1 - x_k
        y_k = g_k+1 - g_k
        rho_k = 1 / (s_k^T * y_k)
        V_k^T = I - rho_k * s_k * y_k^T
        V_k = I - rho_k * y_k * s_k^T
        H_k+1 = V_k^T * H_k * V_k + rho_k * s_k * s_k^T
        check_converge
    end 

    Args:
        objective_func: the objective function to minimize. ``func`` accepts
            a multivariate input and returns a scalar.
        initial_position (Tensor): the starting point of the iterates. For methods like Newton and quasi-Newton 
        the initial trial step length should always be 1.0.
        max_iters (int): the maximum number of minimization iterations.
        tolerance_grad (float): terminates if the gradient norm is smaller than this. Currently gradient norm uses inf norm.
        tolerance_change (float): terminates if the change of function value/position/parameter between 
            two iterations is smaller than this value.
        initial_inverse_hessian_estimate (Tensor): the initial inverse hessian approximation at initial_position.
        It must be symmetric and positive definite.
        line_search_fn (str): indicate which line search method to use, only support 'strong wolfe' right now. May support 
            'Hager Zhang' in the futrue.
        max_line_search_iters (int): the maximum number of line search iterations.
        initial_step_length (float): step length used in first iteration of line search. different initial_step_length 
        may cause different optimal result.
        dtype ('float32' | 'float64'): In static graph, float64 will be convert to float32 due to paddle.assign limit.
    
    Returns:
        is_converge (bool): Indicates whether found the minimum within tolerance.
        num_func_calls (int): number of objective function called.
        position (Tensor): the position of the last iteration. If the search converged, this value is the argmin of 
        the objective function regrading to the initial position.
        objective_value (Tensor): objective function value at the `position`.
        objective_gradient (Tensor): objective function gradient at the `position`.
        inverse_hessian_estimate (Tensor): the estimate of inverse hessian at the `position`.

    Examples:
        .. code-block:: python

            import paddle
            
            def func(x):
                return paddle.dot(x, x)

            x0 = paddle.to_tensor([1.3, 2.7])
            results = paddle.incubate.optimizer.functional.minimize_bfgs(func, x0)
            print("is_converge: ", results[0])
            print("the minimum of func is: ", results[2])
            # is_converge:  is_converge:  Tensor(shape=[1], dtype=bool, place=Place(gpu:0), stop_gradient=True,
            #        [True])
            # the minimum of func is:  Tensor(shape=[2], dtype=float32, place=Place(gpu:0), stop_gradient=True,
            #        [0., 0.])
    """

    if dtype not in ['float32', 'float64']:
        raise ValueError(
            "The dtype must be 'float32' or 'float64', but the specified is {}.".
            format(dtype))

    op_name = 'minimize_bfgs'
    check_input_type(initial_position, 'initial_position', op_name)

    I = paddle.eye(initial_position.shape[0], dtype=dtype)
    if initial_inverse_hessian_estimate is None:
        initial_inverse_hessian_estimate = I
    else:
        check_input_type(initial_inverse_hessian_estimate,
                         'initial_inverse_hessian_estimate', op_name)
        check_initial_inverse_hessian_estimate(initial_inverse_hessian_estimate)

    Hk = paddle.assign(initial_inverse_hessian_estimate)
    xk = initial_position

    value, g1 = _value_and_gradient(objective_func, xk)
    num_func_calls = paddle.full(shape=[1], fill_value=1, dtype='int64')

    # when the dim of x is 1000, it needs more than 30 iters to get all element converge to minimum.
    k = paddle.full(shape=[1], fill_value=0, dtype='int64')
    done = paddle.full(shape=[1], fill_value=False, dtype='bool')
    is_converge = paddle.full(shape=[1], fill_value=False, dtype='bool')

    def cond(k, done, is_converge, num_func_calls, xk, value, g1, Hk):
        return (k < max_iters) & ~done

    def body(k, done, is_converge, num_func_calls, xk, value, g1, Hk):
        #############    compute pk    #############
        pk = -paddle.matmul(Hk, g1)

        #############    compute alpha by line serach    #############
        if line_search_fn == 'strong_wolfe':
            alpha, value, g2, ls_func_calls = strong_wolfe(
                f=objective_func,
                xk=xk,
                pk=pk,
                initial_step_length=initial_step_length,
                dtype=dtype)
        else:
            raise NotImplementedError(
                "Currently only support line_search_fn = 'strong_wolfe', but the specified is '{}'".
                format(line_search_fn))
        num_func_calls += ls_func_calls

        #############    update Hk    #############
        sk = alpha * pk
        yk = g2 - g1

        xk = xk + sk
        g1 = g2

        sk = paddle.unsqueeze(sk, 0)
        yk = paddle.unsqueeze(yk, 0)

        rhok_inv = paddle.dot(yk, sk)
        rhok = paddle.static.nn.cond(
            rhok_inv == 0., lambda: paddle.full(shape=[1], fill_value=1000.0, dtype=dtype), lambda: 1. / rhok_inv)

        Vk_transpose = I - rhok * sk * yk.t()
        Vk = I - rhok * yk * sk.t()
        Hk = paddle.matmul(paddle.matmul(Vk_transpose, Hk),
                           Vk) + rhok * sk * sk.t()

        k += 1

        #############    check convergence    #############
        gnorm = paddle.linalg.norm(g1, p=np.inf)
        pk_norm = paddle.linalg.norm(pk, p=np.inf)
        paddle.assign(done | (gnorm < tolerance_grad) |
                      (pk_norm < tolerance_change), done)
        paddle.assign(done, is_converge)
        # when alpha=0, there is no chance to get xk change.
        paddle.assign(done | (alpha == 0.), done)
        return [k, done, is_converge, num_func_calls, xk, value, g1, Hk]

    paddle.static.nn.while_loop(
        cond=cond,
        body=body,
        loop_vars=[k, done, is_converge, num_func_calls, xk, value, g1, Hk])
    return is_converge, num_func_calls, xk, value, g1, Hk
