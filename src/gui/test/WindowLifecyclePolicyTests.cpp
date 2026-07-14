/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/WindowLifecyclePolicy.h"

#include <gtest/gtest.h>

TEST(WindowLifecyclePolicyTests, onlyExplicitQuitMayCloseTheApplication)
{
    EXPECT_EQ(WindowLifecyclePolicy::CloseAction::Quit,
              WindowLifecyclePolicy::closeAction(true, true));
    EXPECT_EQ(WindowLifecyclePolicy::CloseAction::Quit,
              WindowLifecyclePolicy::closeAction(true, false));
}

TEST(WindowLifecyclePolicyTests, implicitCloseKeepsTheCoreAlive)
{
    EXPECT_EQ(WindowLifecyclePolicy::CloseAction::Hide,
              WindowLifecyclePolicy::closeAction(false, true));
    EXPECT_EQ(WindowLifecyclePolicy::CloseAction::Minimize,
              WindowLifecyclePolicy::closeAction(false, false));
}

TEST(WindowLifecyclePolicyTests, combinedWindowStateStillCountsAsMinimized)
{
    const Qt::WindowStates minimizedState =
        Qt::WindowMinimized | Qt::WindowActive;

    EXPECT_TRUE(WindowLifecyclePolicy::shouldHideOnMinimize(
        minimizedState, true, true));
    EXPECT_FALSE(WindowLifecyclePolicy::shouldHideOnMinimize(
        minimizedState, false, true));
    EXPECT_FALSE(WindowLifecyclePolicy::shouldHideOnMinimize(
        minimizedState, true, false));
    EXPECT_FALSE(WindowLifecyclePolicy::shouldHideOnMinimize(
        Qt::WindowActive, true, true));
}
