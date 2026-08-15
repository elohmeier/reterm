import { mount } from 'svelte';
import App from './App.svelte';

const target = document.getElementById('app')!;
// The device-served shell puts placeholder text here; mount() appends.
target.textContent = '';
mount(App, { target });
