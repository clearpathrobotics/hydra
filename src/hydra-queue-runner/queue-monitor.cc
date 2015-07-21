#include "state.hh"
#include "build-result.hh"

using namespace nix;


void State::queueMonitor()
{
    while (true) {
        try {
            queueMonitorLoop();
        } catch (std::exception & e) {
            printMsg(lvlError, format("queue monitor: %1%") % e.what());
            sleep(10); // probably a DB problem, so don't retry right away
        }
    }
}


void State::queueMonitorLoop()
{
    auto conn(dbPool.get());

    receiver buildsAdded(*conn, "builds_added");
    receiver buildsRestarted(*conn, "builds_restarted");
    receiver buildsCancelled(*conn, "builds_cancelled");
    receiver buildsDeleted(*conn, "builds_deleted");

    auto store = openStore(); // FIXME: pool

    unsigned int lastBuildId = 0;

    while (true) {
        getQueuedBuilds(*conn, store, lastBuildId);

        /* Sleep until we get notification from the database about an
           event. */
        conn->await_notification();
        nrQueueWakeups++;

        if (buildsAdded.get())
            printMsg(lvlTalkative, "got notification: new builds added to the queue");
        if (buildsRestarted.get()) {
            printMsg(lvlTalkative, "got notification: builds restarted");
            lastBuildId = 0; // check all builds
        }
        if (buildsCancelled.get() || buildsDeleted.get()) {
            printMsg(lvlTalkative, "got notification: builds cancelled");
            removeCancelledBuilds(*conn);
        }

    }
}


void State::getQueuedBuilds(Connection & conn, std::shared_ptr<StoreAPI> store, unsigned int & lastBuildId)
{
    printMsg(lvlInfo, format("checking the queue for builds > %1%...") % lastBuildId);

    /* Grab the queued builds from the database, but don't process
       them yet (since we don't want a long-running transaction). */
    std::multimap<Path, Build::ptr> newBuilds;

    {
        pqxx::work txn(conn);

        auto res = txn.parameterized("select id, project, jobset, job, drvPath, maxsilent, timeout from Builds where id > $1 and finished = 0 order by id")(lastBuildId).exec();

        for (auto const & row : res) {
            auto builds_(builds.lock());
            BuildID id = row["id"].as<BuildID>();
            if (buildOne && id != buildOne) continue;
            if (id > lastBuildId) lastBuildId = id;
            if (has(*builds_, id)) continue;

            auto build = std::make_shared<Build>();
            build->id = id;
            build->drvPath = row["drvPath"].as<string>();
            build->fullJobName = row["project"].as<string>() + ":" + row["jobset"].as<string>() + ":" + row["job"].as<string>();
            build->maxSilentTime = row["maxsilent"].as<int>();
            build->buildTimeout = row["timeout"].as<int>();

            newBuilds.emplace(std::make_pair(build->drvPath, build));
        }
    }

    std::set<Step::ptr> newRunnable;
    unsigned int nrAdded;
    std::function<void(Build::ptr)> createBuild;

    createBuild = [&](Build::ptr build) {
        printMsg(lvlTalkative, format("loading build %1% (%2%)") % build->id % build->fullJobName);
        nrAdded++;

        if (!store->isValidPath(build->drvPath)) {
            /* Derivation has been GC'ed prematurely. */
            printMsg(lvlError, format("aborting GC'ed build %1%") % build->id);
            if (!build->finishedInDB) {
                pqxx::work txn(conn);
                txn.parameterized
                    ("update Builds set finished = 1, busy = 0, buildStatus = $2, startTime = $3, stopTime = $3, errorMsg = $4 where id = $1 and finished = 0")
                    (build->id)
                    ((int) bsAborted)
                    (time(0))
                    ("derivation was garbage-collected prior to build").exec();
                txn.commit();
                build->finishedInDB = true;
                nrBuildsDone++;
            }
            return;
        }

        std::set<Step::ptr> newSteps;
        std::set<Path> finishedDrvs; // FIXME: re-use?
        Step::ptr step = createStep(store, build->drvPath, build, 0, finishedDrvs, newSteps, newRunnable);

        /* Some of the new steps may be the top level of builds that
           we haven't processed yet. So do them now. This ensures that
           if build A depends on build B with top-level step X, then X
           will be "accounted" to B in doBuildStep(). */
        for (auto & r : newSteps) {
            while (true) {
                auto i = newBuilds.find(r->drvPath);
                if (i == newBuilds.end()) break;
                Build::ptr b = i->second;
                newBuilds.erase(i);
                createBuild(b);
            }
        }

        /* If we didn't get a step, it means the step's outputs are
           all valid. So we mark this as a finished, cached build. */
        if (!step) {
            Derivation drv = readDerivation(build->drvPath);
            BuildOutput res = getBuildOutput(store, drv);

            pqxx::work txn(conn);
            time_t now = time(0);
            markSucceededBuild(txn, build, res, true, now, now);
            txn.commit();

            build->finishedInDB = true;

            return;
        }

        /* If any step has an unsupported system type or has a
           previously failed output path, then fail the build right
           away. */
        bool badStep = false;
        for (auto & r : newSteps) {
            BuildStatus buildStatus = bsSuccess;
            BuildStepStatus buildStepStatus = bssFailed;

            if (checkCachedFailure(r, conn)) {
                printMsg(lvlError, format("marking build %1% as cached failure") % build->id);
                buildStatus = step == r ? bsFailed : bsDepFailed;
                buildStepStatus = bssFailed;
            }

            if (buildStatus == bsSuccess) {
                bool supported = false;
                {
                    auto machines_(machines.lock()); // FIXME: use shared_mutex
                    for (auto & m : *machines_)
                        if (m.second->supportsStep(r)) { supported = true; break; }
                }

                if (!supported) {
                    printMsg(lvlError, format("aborting unsupported build %1%") % build->id);
                    buildStatus = bsUnsupported;
                    buildStepStatus = bssUnsupported;
                }
            }

            if (buildStatus != bsSuccess) {
                time_t now = time(0);
                if (!build->finishedInDB) {
                    pqxx::work txn(conn);
                    createBuildStep(txn, 0, build, r, "", buildStepStatus);
                    txn.parameterized
                        ("update Builds set finished = 1, busy = 0, buildStatus = $2, startTime = $3, stopTime = $3, isCachedBuild = $4 where id = $1 and finished = 0")
                        (build->id)
                        ((int) buildStatus)
                        (now)
                        (buildStatus != bsUnsupported ? 1 : 0).exec();
                    txn.commit();
                    build->finishedInDB = true;
                    nrBuildsDone++;
                }
                badStep = true;
                break;
            }
        }

        if (badStep) return;

        /* Note: if we exit this scope prior to this, the build and
           all newly created steps are destroyed. */

        {
            auto builds_(builds.lock());
            if (!build->finishedInDB) // FIXME: can this happen?
                (*builds_)[build->id] = build;
            build->toplevel = step;
        }

        printMsg(lvlChatty, format("added build %1% (top-level step %2%, %3% new steps)")
            % build->id % step->drvPath % newSteps.size());
    };

    /* Now instantiate build steps for each new build. The builder
       threads can start building the runnable build steps right away,
       even while we're still processing other new builds. */
    while (!newBuilds.empty()) {
        auto build = newBuilds.begin()->second;
        newBuilds.erase(newBuilds.begin());

        newRunnable.clear();
        nrAdded = 0;
        try {
            createBuild(build);
        } catch (Error & e) {
            e.addPrefix(format("while loading build %1%: ") % build->id);
            throw;
        }

        /* Add the new runnable build steps to ‘runnable’ and wake up
           the builder threads. */
        printMsg(lvlChatty, format("got %1% new runnable steps from %2% new builds") % newRunnable.size() % nrAdded);
        for (auto & r : newRunnable)
            makeRunnable(r);

        nrBuildsRead += nrAdded;
    }
}


void State::removeCancelledBuilds(Connection & conn)
{
    /* Get the current set of queued builds. */
    std::set<BuildID> currentIds;
    {
        pqxx::work txn(conn);
        auto res = txn.exec("select id from Builds where finished = 0");
        for (auto const & row : res)
            currentIds.insert(row["id"].as<BuildID>());
    }

    auto builds_(builds.lock());

    for (auto i = builds_->begin(); i != builds_->end(); ) {
        if (currentIds.find(i->first) == currentIds.end()) {
            printMsg(lvlInfo, format("discarding cancelled build %1%") % i->first);
            i = builds_->erase(i);
            // FIXME: ideally we would interrupt active build steps here.
        } else
            ++i;
    }
}


Step::ptr State::createStep(std::shared_ptr<StoreAPI> store, const Path & drvPath,
    Build::ptr referringBuild, Step::ptr referringStep, std::set<Path> & finishedDrvs,
    std::set<Step::ptr> & newSteps, std::set<Step::ptr> & newRunnable)
{
    if (finishedDrvs.find(drvPath) != finishedDrvs.end()) return 0;

    /* Check if the requested step already exists. If not, create a
       new step. In any case, make the step reachable from
       referringBuild or referringStep. This is done atomically (with
       ‘steps’ locked), to ensure that this step can never become
       reachable from a new build after doBuildStep has removed it
       from ‘steps’. */
    Step::ptr step;
    bool isNew = false;
    {
        auto steps_(steps.lock());

        /* See if the step already exists in ‘steps’ and is not
           stale. */
        auto prev = steps_->find(drvPath);
        if (prev != steps_->end()) {
            step = prev->second.lock();
            /* Since ‘step’ is a strong pointer, the referred Step
               object won't be deleted after this. */
            if (!step) steps_->erase(drvPath); // remove stale entry
        }

        /* If it doesn't exist, create it. */
        if (!step) {
            step = std::make_shared<Step>();
            step->drvPath = drvPath;
            isNew = true;
        }

        auto step_(step->state.lock());

        assert(step_->created != isNew);

        if (referringBuild)
            step_->builds.push_back(referringBuild);

        if (referringStep)
            step_->rdeps.push_back(referringStep);

        (*steps_)[drvPath] = step;
    }

    if (!isNew) return step;

    printMsg(lvlDebug, format("considering derivation ‘%1%’") % drvPath);

    /* Initialize the step. Note that the step may be visible in
       ‘steps’ before this point, but that doesn't matter because
       it's not runnable yet, and other threads won't make it
       runnable while step->created == false. */
    step->drv = readDerivation(drvPath);
    {
        auto i = step->drv.env.find("requiredSystemFeatures");
        if (i != step->drv.env.end())
            step->requiredSystemFeatures = tokenizeString<std::set<std::string>>(i->second);
    }

    auto attr = step->drv.env.find("preferLocalBuild");
    step->preferLocalBuild =
        attr != step->drv.env.end() && attr->second == "1"
        && has(localPlatforms, step->drv.platform);

    /* Are all outputs valid? */
    bool valid = true;
    for (auto & i : step->drv.outputs) {
        if (!store->isValidPath(i.second.path)) {
            valid = false;
            break;
        }
    }

    // FIXME: check whether all outputs are in the binary cache.
    if (valid) {
        finishedDrvs.insert(drvPath);
        return 0;
    }

    /* No, we need to build. */
    printMsg(lvlDebug, format("creating build step ‘%1%’") % drvPath);
    newSteps.insert(step);

    /* Create steps for the dependencies. */
    for (auto & i : step->drv.inputDrvs) {
        auto dep = createStep(store, i.first, 0, step, finishedDrvs, newSteps, newRunnable);
        if (dep) {
            auto step_(step->state.lock());
            step_->deps.insert(dep);
        }
    }

    /* If the step has no (remaining) dependencies, make it
       runnable. */
    {
        auto step_(step->state.lock());
        assert(!step_->created);
        step_->created = true;
        if (step_->deps.empty())
            newRunnable.insert(step);
    }

    return step;
}
