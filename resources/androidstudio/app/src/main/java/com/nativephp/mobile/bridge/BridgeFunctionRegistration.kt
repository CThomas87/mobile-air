package com.nativephp.mobile.bridge

import android.content.Context
import androidx.fragment.app.FragmentActivity
import com.nativephp.mobile.bridge.functions.EdgeFunctions
import com.nativephp.mobile.bridge.functions.WorkerFunctions
import com.nativephp.mobile.bridge.plugins.registerPluginBridgeFunctions

/**
 * Register all bridge functions with the registry
 * Call this once during app initialization
 */
fun registerBridgeFunctions(activity: FragmentActivity, context: Context) {
    val registry = BridgeFunctionRegistry.shared

    registry.register("Edge.Set", EdgeFunctions.Set())

    // Worker control functions (called from PHP via nativephp_call)
    registry.register("Worker.Start",  WorkerFunctions.Start(context))
    registry.register("Worker.Stop",   WorkerFunctions.Stop(context))
    registry.register("Worker.Status", WorkerFunctions.Status())

    // Register plugin bridge functions
    registerPluginBridgeFunctions(activity, context)
}